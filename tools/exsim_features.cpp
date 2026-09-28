// exsim_features: order-book state and order flow from a day of Coinbase L3 data, for the signal study.
//
// Rebuilds the exact book from the exchange's own messages (the truth book in l3.hpp; no matching logic),
// and writes two files:
//
//   <prefix>.bars.csv   one row per bar (default 1 s of exchange time), describing the book at the END of the
//                       bar and the flow DURING it:
//                         t_ns, bid, ask (ticks), bid_qty, ask_qty (best level, lots), bid5, ask5 (top five
//                         levels), ofi (order-flow imbalance at the best quotes, Cont, Kukanov and Stoikov
//                         2014, lots), buy_vol, sell_vol (traded volume by aggressor side, lots), events,
//                         valid (0 if the bar touched a gap in the feed)
//   <prefix>.moves.csv  every change of the mid price: t_ns, mid2 (bid + ask, ticks; twice the mid, so exact)
//
// Features are computed from information available at the time; targets (the next mid move, future
// returns) are joined later in scripts/analyze_signals.py, so no look-ahead can leak in here.
//
//   exsim_features --in 2026-09-01.exl3 --out-prefix results/signals/2026-09-01 [--bar-ms 1000]

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

#include "args.hpp"
#include "exsim/l3.hpp"

using namespace exsim;
using l3::Rec;

namespace {

class Features {
 public:
  Features(std::uint64_t bar_ns, std::FILE* bars, std::FILE* moves) : bar_ns_(bar_ns), bars_(bars), moves_(moves) {
    std::fprintf(bars_, "t_ns,bid,ask,bid_qty,ask_qty,bid5,ask5,ofi,buy_vol,sell_vol,events,valid\n");
    std::fprintf(moves_, "t_ns,mid2\n");
  }

  void on(const Rec& r) {
    // Snapshots: the first seeds the book; a later one (Tardis reconnected) is used only to recover from a gap.
    if (r.kind == l3::SnapOrder) {
      if (synced_) return;
      if (!loading_) truth_.clear(), loading_ = true;
      truth_.add(r.id, r.side ? Side::Sell : Side::Buy, r.px, r.q, 0);
      return;
    }
    if (r.kind == l3::SnapEnd) {
      if (synced_) return;
      loading_ = false, synced_ = true;
      snap_seq_ = last_seq_ = r.seq;
      have_prev_ = false;  // OFI restarts from the snapshot state
      std::vector<Rec> early;
      early.swap(pre_);
      std::sort(early.begin(), early.end(), [](const Rec& a, const Rec& b) { return a.seq < b.seq; });
      for (const Rec& e : early)
        if (e.seq > snap_seq_) on(e);
      return;
    }
    if (!synced_) {
      pre_.push_back(r);
      return;
    }
    if (r.seq <= snap_seq_) return;
    if (r.seq != last_seq_ + 1) {  // messages missing: nothing is trustworthy until the next snapshot
      ++gaps_;
      synced_ = false;
      gap_from_ = last_ts_, gap_to_ = UINT64_MAX;
      pre_.push_back(r);
      return;
    }
    last_seq_ = r.seq;
    if (gap_to_ == UINT64_MAX) gap_to_ = r.ts;  // first message applied after a resynchronization
    last_ts_ = r.ts;
    roll_bars(r.ts);
    apply(r);
    ++events_;
    observe(r.ts);
  }

  void finish() { flush_bar(); }
  std::uint64_t gaps() const { return gaps_; }
  std::uint64_t bars() const { return nbars_; }
  std::uint64_t moves() const { return nmoves_; }

 private:
  void apply(const Rec& r) {
    const Side side = r.side ? Side::Sell : Side::Buy;
    switch (r.kind) {
      case l3::Open: truth_.add(r.id, side, r.px, r.q, r.seq); break;
      case l3::Match:
        truth_.fill(r.id2, r.q);
        // r.side is the maker's side: a resting sell was hit by a buyer.
        (side == Side::Sell ? buy_vol_ : sell_vol_) += r.q;
        break;
      case l3::Done: truth_.remove(r.id); break;
      case l3::Change:
        if (!(r.flags & l3::FundsChange)) truth_.change(r.id, r.px, r.q, r.seq);
        break;
      default: break;
    }
  }

  // Order-flow imbalance increment at the best quotes (Cont, Kukanov and Stoikov 2014, eq. 2): bid-side
  // arrivals and ask-side departures push the price up; the reverse pushes it down.
  void observe(std::uint64_t ts) {
    const auto b = truth_.best(Side::Buy), a = truth_.best(Side::Sell);
    if (!b || !a) return;
    const Qty qb = truth_.level_qty(Side::Buy, *b), qa = truth_.level_qty(Side::Sell, *a);
    if (have_prev_) {
      double e = 0;
      if (*b >= pb_) e += static_cast<double>(qb);
      if (*b <= pb_) e -= static_cast<double>(qb_);
      if (*a <= pa_) e -= static_cast<double>(qa);
      if (*a >= pa_) e += static_cast<double>(qa_);
      ofi_ += e;
      if (*b + *a != pb_ + pa_) {
        std::fprintf(moves_, "%" PRIu64 ",%" PRId64 "\n", ts, *b + *a);
        ++nmoves_;
      }
    }
    have_prev_ = true;
    pb_ = *b, pa_ = *a, qb_ = qb, qa_ = qa;
  }

  // Emits every bar that ended before ts (empty bars repeat the book state with zero flow).
  void roll_bars(std::uint64_t ts) {
    if (bar_end_ == 0) {
      bar_end_ = (ts / bar_ns_ + 1) * bar_ns_;
      return;
    }
    while (ts >= bar_end_) {
      flush_bar();
      bar_end_ += bar_ns_;
    }
  }

  void flush_bar() {
    if (bar_end_ == 0 || !have_prev_) return;
    Qty b5 = 0, a5 = 0;
    truth_.for_each_level(Side::Buy, 5, [&](Price, Qty q) { b5 += q; });
    truth_.for_each_level(Side::Sell, 5, [&](Price, Qty q) { a5 += q; });
    // A bar is valid if it lies entirely before the last gap or entirely after the book was resynchronized.
    const bool valid = bar_end_ <= gap_from_ || bar_end_ - bar_ns_ >= gap_to_;
    std::fprintf(bars_, "%" PRIu64 ",%" PRId64 ",%" PRId64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.0f,%" PRIu64
                        ",%" PRIu64 ",%" PRIu64 ",%d\n",
                 bar_end_, pb_, pa_, qb_, qa_, b5, a5, ofi_, buy_vol_, sell_vol_, events_, valid ? 1 : 0);
    ++nbars_;
    ofi_ = 0, buy_vol_ = sell_vol_ = events_ = 0;
  }

  l3::TruthBook truth_;
  std::uint64_t bar_ns_;
  std::FILE* bars_;
  std::FILE* moves_;
  bool loading_ = false, synced_ = false, have_prev_ = false;
  std::uint64_t snap_seq_ = 0, last_seq_ = 0, gaps_ = 0, bar_end_ = 0, nbars_ = 0, nmoves_ = 0, last_ts_ = 0;
  std::uint64_t gap_from_ = UINT64_MAX, gap_to_ = 0;  // exchange-time window of the last gap (none: all valid)
  std::vector<Rec> pre_;
  Price pb_ = 0, pa_ = 0;
  Qty qb_ = 0, qa_ = 0, buy_vol_ = 0, sell_vol_ = 0;
  std::uint64_t events_ = 0;
  double ofi_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  const tools::Args args(argc, argv);
  if (!args.has("in") || !args.has("out-prefix")) tools::Args::die("--in <file.exl3> and --out-prefix are required");
  const std::string prefix = args.str("out-prefix", "");
  std::FILE* bars = std::fopen((prefix + ".bars.csv").c_str(), "w");
  std::FILE* moves = std::fopen((prefix + ".moves.csv").c_str(), "w");
  if (bars == nullptr || moves == nullptr) tools::Args::die("cannot write " + prefix + ".*.csv");
  Features f(args.u64("bar-ms", 1000) * 1'000'000, bars, moves);
  l3::Reader rd(args.str("in", ""));
  while (const Rec* r = rd.next()) f.on(*r);
  f.finish();
  std::fclose(bars);
  std::fclose(moves);
  std::printf("bars %" PRIu64 ", mid moves %" PRIu64 ", feed gaps %" PRIu64 "\n", f.bars(), f.moves(), f.gaps());
  return 0;
}
