// exsim_queuestudy: how well can an L2 observer estimate queue position? Ground truth from L3.
//
// A passive order's fill depends on how much quantity is AHEAD of it in the FIFO queue at its price. With
// only aggregate (L2) data that is unobservable: when the level shrinks without a trade, nobody can tell
// whether the cancelled quantity was ahead of you or behind you. Backtesters therefore assume a rule, and
// Danait, Zamora & Boier (arXiv:2609.13597, 2026) show the rule moves results materially. With Coinbase's
// order-by-order feed the queue is known exactly, so the rules can be scored against the truth.
//
// Two measurements over a day of data:
//
//  1. WHERE CANCELLATIONS COME FROM. For every cancellation or size reduction of a resting order at the best
//     bid or ask, its relative queue position = quantity ahead of it / level quantity (0 front, 1 back).
//     Under the common "uniform" assumption this is uniformly distributed.
//
//  2. VIRTUAL PROBES. Every --probe-every seconds, a zero-size probe joins the back of the best bid and the
//     best ask. Its TRUE fill time is exact: the first time a trade at its price hits an order that joined
//     after it (the taker passed its position), or a trade prints through its price. Each estimator sees only
//     what an L2 observer sees (level quantity changes and trade prints) and predicts the same fill:
//        front   cancelled quantity is always ahead of us (the optimistic bound)
//        back    cancelled quantity is always behind us unless it exceeds what is behind (pessimistic bound)
//        prop    cancels are spread uniformly: share ahead = ahead / level        (= power 1)
//        pow2 / pow3   share ahead = ahead^n / (ahead^n + behind^n)   (hftbacktest-style power rules)
//        log     share ahead = log(1+ahead) / (log(1+ahead) + log(1+behind))
//     Trades always consume the queue from the front.
//
// Outputs: summary to stdout; per-probe CSV and cancel-position histogram CSV with --out-prefix.
//
//   exsim_queuestudy --in day.exl3 [--probe-every 5] [--horizon 300] [--out-prefix results/queue/2026-09-01]

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "args.hpp"
#include "exsim/l3.hpp"

using namespace exsim;
using l3::Rec;

namespace {

constexpr int kRules = 6;
const char* kRuleName[kRules] = {"front", "prop", "pow2", "pow3", "log", "back"};

// Share of a cancellation of size c attributed to the queue ahead of the probe, for each rule.
double ahead_share(int rule, double a, double b) {
  if (a <= 0) return 0;
  if (b <= 0) return 1;
  switch (rule) {
    case 0: return 1.0;                                   // front: all of it (capped by a below)
    case 1: return a / (a + b);                           // proportional
    case 2: return a * a / (a * a + b * b);               // power 2
    case 3: return a * a * a / (a * a * a + b * b * b);   // power 3
    case 4: return std::log1p(a) / (std::log1p(a) + std::log1p(b));
    default: return 0.0;                                  // back: none, unless it exceeds b (below)
  }
}

struct Probe {
  Side side;
  Price px;
  std::uint64_t join_seq;
  std::uint64_t join_ts;
  double level0;                      // level quantity at join (all ahead)
  std::uint32_t n0 = 0;               // number of orders at the level at join
  std::uint8_t fill_kind = 0;         // 0 not filled, 1 filled by a trade at its price, 2 by a trade through it
  std::uint64_t true_fill_ts = 0;     // 0: not filled (yet)
  std::array<double, kRules> ahead{};       // estimated quantity ahead, per rule
  std::array<std::uint64_t, kRules> est_fill_ts{};
  double abs_err_sum[kRules] = {};    // sum over trade events of |est - true| / level
  std::uint64_t err_n = 0;
  bool done = false;
};

class Study {
 public:
  Study(double probe_every_s, double horizon_s) : every_ns_(static_cast<std::uint64_t>(probe_every_s * 1e9)),
                                                  horizon_ns_(static_cast<std::uint64_t>(horizon_s * 1e9)) {}

  void on(const Rec& r) {
    // A later snapshot (Tardis reconnected) is skipped while in sync, and after a gap it is the way back in.
    if (r.kind == l3::SnapOrder) {
      if (synced_) return;
      if (!loading_) { truth_.clear(); loading_ = true; }
      truth_.add(r.id, r.side ? Side::Sell : Side::Buy, r.px, r.q, 0);  // snapshot orders precede every probe
      return;
    }
    if (r.kind == l3::SnapEnd) {
      if (synced_) return;
      loading_ = false;
      synced_ = true;
      snap_seq_ = last_seq_ = r.seq;
      std::vector<Rec> early;
      early.swap(pre_);
      std::sort(early.begin(), early.end(), [](const Rec& a, const Rec& b) { return a.seq < b.seq; });
      for (const Rec& e : early)
        if (e.seq > snap_seq_) on(e);
      return;
    }
    if (!synced_) { pre_.push_back(r); return; }
    if (r.seq <= snap_seq_) return;
    if (r.seq != last_seq_ + 1) {
      on_gap();
      pre_.push_back(r);
      return;
    }
    last_seq_ = r.seq;
    now_ = r.ts;
    apply(r);
    maybe_place_probes(r);
    expire_probes();
  }

  void finish(const std::string& prefix) const;
  std::uint64_t gaps() const { return gaps_; }
  std::uint64_t voided() const { return voided_; }
  bool synced() const { return synced_; }

 private:
  // Messages are missing: the queue ahead of every open probe is unknown, so those probes are dropped (not
  // scored either way), and the study waits for the next snapshot.
  void on_gap() {
    ++gaps_;
    std::vector<bool> open(probes_.size(), false);
    for (const std::size_t pi : active_) open[pi] = true;
    std::vector<Probe> keep;
    keep.reserve(probes_.size());
    for (std::size_t i = 0; i < probes_.size(); ++i)
      if (!open[i]) keep.push_back(probes_[i]);
    voided_ += probes_.size() - keep.size();
    probes_.swap(keep);
    active_.clear();
    at_price_.clear();
    synced_ = false;
    next_probe_ = 0;  // warm up again after the snapshot
  }

  // ---------------- truth book updates, with L2-visible deltas routed to probes ----------------
  void apply(const Rec& r) {
    const Side side = r.side ? Side::Sell : Side::Buy;
    switch (r.kind) {
      case l3::Open:
        truth_.add(r.id, side, r.px, r.q, r.seq);
        break;  // additions join behind every probe at this price: no estimator change
      case l3::Match: {
        Side ms;
        Price mpx;
        const auto* mk = truth_.find(r.id2, &ms, &mpx);
        const std::uint64_t maker_since = mk ? mk->since : 0;
        truth_.fill(r.id2, r.q);
        on_trade(side, r.px, static_cast<double>(r.q), maker_since);
        break;
      }
      case l3::Done: {
        Side s;
        Price px;
        const auto* o = truth_.find(r.id, &s, &px);
        if (o != nullptr && o->qty > 0 && (r.flags & l3::Canceled)) on_cancel(s, px, r.id, static_cast<double>(o->qty));
        truth_.remove(r.id);
        break;
      }
      case l3::Change: {
        if (r.flags & l3::FundsChange) break;
        Side s;
        Price px;
        const auto* o = truth_.find(r.id, &s, &px);
        if (o == nullptr) break;
        const Price new_px = r.px ? r.px : px;
        const double old_q = static_cast<double>(o->qty);
        if (new_px == px && r.q <= o->qty) {  // in-place decrease: keeps position, L2 sees a cancel
          if (r.q < o->qty) on_cancel(s, px, r.id, old_q - static_cast<double>(r.q));
          truth_.change(r.id, new_px, r.q, r.seq);
        } else {  // leaves this level (L2: cancel of all of it) and rejoins at the back (possibly elsewhere)
          on_cancel(s, px, r.id, old_q);
          truth_.change(r.id, new_px, r.q, r.seq);
        }
        break;
      }
      default: break;
    }
  }

  // Quantity ahead of a probe that joined at join_seq: orders at its level that joined before it.
  double true_ahead(const Probe& p) const {
    const auto* q = truth_.queue(p.side, p.px);
    if (q == nullptr) return 0;
    double a = 0;
    for (const auto& o : *q) {
      if (o.since >= p.join_seq) break;  // FIFO: everything from here on joined after the probe
      a += static_cast<double>(o.qty);
    }
    return a;
  }

  double level_qty(Side s, Price px) const {
    const auto* q = truth_.queue(s, px);
    double t = 0;
    if (q) for (const auto& o : *q) t += static_cast<double>(o.qty);
    return t;
  }

  // A cancellation (or size reduction) of `c` at (s, px) by order `id`.
  void on_cancel(Side s, Price px, OrderId id, double c) {
    // 1. where did it come from? (only at the touch, before the order is removed)
    const auto best = truth_.best(s);
    if (best && *best == px) {
      const auto* q = truth_.queue(s, px);
      double ahead = 0, total = 0;
      bool found = false;
      for (const auto& o : *q) {
        if (o.id == id) found = true;
        if (!found) ahead += static_cast<double>(o.qty);
        total += static_cast<double>(o.qty);
      }
      if (found && total > 0) {
        const double rel = ahead / total;  // 0 front .. <1 back (position of the cancelled order's head)
        ++cancel_hist_[std::min(9, static_cast<int>(rel * 10))];
        ++cancels_at_touch_;
        std::size_t n = 0, pos = 0;
        for (const auto& o : *q) { if (o.id == id) pos = n; ++n; }
        // rank (by count) among the level's orders; under the null "every resting order is equally likely
        // to cancel" the rank is uniform on 0..n-1, so rank deciles are uniform for any n.
        const int bucket = n == 1 ? 0 : n <= 4 ? 1 : n <= 9 ? 2 : 3;
        ++rank_hist_[bucket][std::min<std::size_t>(9, 10 * pos / n)];
        ++bucket_n_[bucket];
        if (n > 1) {  // normalized rank: 0 front, 1 back; mean 0.5 under the null
          rank_sum_[bucket] += static_cast<double>(pos) / static_cast<double>(n - 1);
          if (2 * pos < n - 1) ++front_half_[bucket];
          else if (2 * pos > n - 1) ++back_half_[bucket];
        }
      }
    }
    // 2. estimators of every active probe at this price (they see only the size of the reduction)
    auto it = at_price_.find(key(s, px));
    if (it == at_price_.end()) return;
    const double level_before = level_qty(s, px);  // the order is still in the book here
    for (std::size_t pi : it->second) {
      Probe& p = probes_[pi];
      if (p.done) continue;
      for (int k = 0; k < kRules; ++k) {
        if (p.est_fill_ts[k]) continue;
        const double a = std::min(p.ahead[k], level_before);
        const double b = std::max(0.0, level_before - a);
        double cut;
        if (k == 5) cut = std::max(0.0, c - b);        // back: only what cannot have come from behind
        else cut = std::min(a, c * ahead_share(k, a, b));
        if (k == 0) cut = std::min(a, c);              // front
        p.ahead[k] = std::max(0.0, a - cut);
      }
    }
  }

  // A trade of q at (maker side s, px). maker_since: queue-join seq of the maker that was hit.
  void on_trade(Side s, Price px, double q, std::uint64_t maker_since) {
    // trade-through: fills every active probe on this side at a better price for the taker's direction
    for (std::size_t pi : active_) {
      Probe& p = probes_[pi];
      if (p.done || p.side != s) continue;
      const bool through = s == Side::Buy ? px < p.px : px > p.px;
      const bool at = px == p.px;
      if (!through && !at) continue;
      if (through) {
        if (!p.true_fill_ts) p.true_fill_ts = now_, p.fill_kind = 2;
        for (int k = 0; k < kRules; ++k)
          if (!p.est_fill_ts[k]) p.est_fill_ts[k] = now_;
        continue;
      }
      // at the probe's price: truth fills when the taker hits an order that joined after the probe
      if (!p.true_fill_ts && maker_since >= p.join_seq) p.true_fill_ts = now_, p.fill_kind = 1;
      const double ta = true_ahead(p) + (maker_since < p.join_seq ? q : 0);  // true ahead before this fill
      for (int k = 0; k < kRules; ++k) {
        if (p.est_fill_ts[k]) continue;
        p.abs_err_sum[k] += std::fabs(p.ahead[k] - ta) / std::max(1.0, p.level0);
        p.ahead[k] -= q;  // trades consume from the front
        if (p.ahead[k] < 0) p.est_fill_ts[k] = now_;
      }
      ++p.err_n;
    }
  }

  // ---------------- probes ----------------
  static std::uint64_t key(Side s, Price px) { return (static_cast<std::uint64_t>(px) << 1) | (s == Side::Sell ? 1 : 0); }

  void maybe_place_probes(const Rec& r) {
    if (now_ < next_probe_) return;
    if (next_probe_ == 0) { next_probe_ = now_ + every_ns_; return; }  // skip the first interval (warm-up)
    next_probe_ += every_ns_;
    (void)r;
    for (Side s : {Side::Buy, Side::Sell}) {
      const auto best = truth_.best(s);
      if (!best) continue;
      Probe p;
      p.side = s, p.px = *best, p.join_seq = last_seq_ + 1, p.join_ts = now_;  // joins after everything so far
      p.level0 = level_qty(s, *best);
      if (const auto* q = truth_.queue(s, *best)) p.n0 = static_cast<std::uint32_t>(q->size());
      for (int k = 0; k < kRules; ++k) p.ahead[k] = p.level0;
      probes_.push_back(p);
      const std::size_t idx = probes_.size() - 1;
      active_.push_back(idx);
      at_price_[key(s, *best)].push_back(idx);
    }
  }

  void expire_probes() {
    if (active_.empty() || now_ < next_expiry_check_) return;
    next_expiry_check_ = now_ + 1'000'000'000ull;
    std::vector<std::size_t> keep;
    for (std::size_t pi : active_) {
      Probe& p = probes_[pi];
      bool all = p.true_fill_ts != 0;
      for (int k = 0; k < kRules && all; ++k) all = p.est_fill_ts[k] != 0;
      if (all || now_ - p.join_ts > horizon_ns_) {
        p.done = true;
        auto& v = at_price_[key(p.side, p.px)];
        v.erase(std::remove(v.begin(), v.end(), pi), v.end());
      } else {
        keep.push_back(pi);
      }
    }
    active_.swap(keep);
  }

  l3::TruthBook truth_;
  bool loading_ = false, synced_ = false;
  std::uint64_t snap_seq_ = 0, last_seq_ = 0, gaps_ = 0, voided_ = 0, now_ = 0;
  std::vector<Rec> pre_;
  std::uint64_t every_ns_, horizon_ns_, next_probe_ = 0, next_expiry_check_ = 0;
  std::vector<Probe> probes_;
  std::vector<std::size_t> active_;
  std::unordered_map<std::uint64_t, std::vector<std::size_t>> at_price_;
  std::array<std::uint64_t, 10> cancel_hist_{};
  std::uint64_t rank_hist_[4][10] = {};
  std::uint64_t bucket_n_[4] = {};
  double rank_sum_[4] = {};
  std::uint64_t front_half_[4] = {}, back_half_[4] = {};
  std::uint64_t cancels_at_touch_ = 0;
};

void Study::finish(const std::string& prefix) const {
  const double H = static_cast<double>(horizon_ns_);
  // per-probe CSV
  if (!prefix.empty()) {
    std::ofstream f(prefix + ".probes.csv");
    f << "side,join_ts,level0,n0,fill_kind,true_fill_s";
    for (int k = 0; k < kRules; ++k) f << ',' << kRuleName[k] << "_fill_s";
    for (int k = 0; k < kRules; ++k) f << ',' << kRuleName[k] << "_err";
    f << '\n';
    for (const Probe& p : probes_) {
      auto secs = [&](std::uint64_t t) { return t && t - p.join_ts <= horizon_ns_ ? static_cast<double>(t - p.join_ts) * 1e-9 : -1.0; };
      f << (p.side == Side::Buy ? "bid" : "ask") << ',' << p.join_ts << ',' << p.level0 << ',' << p.n0 << ','
        << static_cast<int>(p.fill_kind) << ',' << secs(p.true_fill_ts);
      for (int k = 0; k < kRules; ++k) f << ',' << secs(p.est_fill_ts[k]);
      for (int k = 0; k < kRules; ++k) f << ',' << (p.err_n ? p.abs_err_sum[k] / static_cast<double>(p.err_n) : -1.0);
      f << '\n';
    }
    std::ofstream h(prefix + ".cancels.csv");
    h << "orders_in_level,rank_decile,count\n";
    const char* names[4] = {"1", "2-4", "5-9", "10+"};
    for (int b = 0; b < 4; ++b)
      for (int i = 0; i < 10; ++i) h << names[b] << ',' << i << ',' << rank_hist_[b][i] << '\n';
    std::ofstream hq(prefix + ".cancels_qty.csv");
    hq << "qty_decile,count\n";
    for (int i = 0; i < 10; ++i) hq << i << ',' << cancel_hist_[i] << '\n';
  }
  // summary
  std::uint64_t n = 0, true_filled = 0;
  for (const Probe& p : probes_) {
    ++n;
    true_filled += p.true_fill_ts && p.true_fill_ts - p.join_ts <= horizon_ns_;
  }
  std::printf("probes %" PRIu64 " (every %.0f s at the best bid and ask), horizon %.0f s; truly filled within horizon: %" PRIu64
              " (%.1f%%)\n",
              n, static_cast<double>(every_ns_) * 1e-9, H * 1e-9, true_filled, 100.0 * static_cast<double>(true_filled) / static_cast<double>(n));
  std::printf("%-6s %9s %9s %9s %11s %11s %13s %12s\n", "rule", "fills", "precision", "recall", "med err s", "med |err| s",
              "queue err", "fill-rate x");
  for (int k = 0; k < kRules; ++k) {
    std::uint64_t pred = 0, tp = 0;
    std::vector<double> err, aerr, qerr;
    for (const Probe& p : probes_) {
      const bool tf = p.true_fill_ts && p.true_fill_ts - p.join_ts <= horizon_ns_;
      const bool ef = p.est_fill_ts[k] && p.est_fill_ts[k] - p.join_ts <= horizon_ns_;
      pred += ef;
      tp += tf && ef;
      if (tf && ef) {
        const double e = (static_cast<double>(p.est_fill_ts[k]) - static_cast<double>(p.true_fill_ts)) * 1e-9;
        err.push_back(e), aerr.push_back(std::fabs(e));
      }
      if (p.err_n) qerr.push_back(p.abs_err_sum[k] / static_cast<double>(p.err_n));
    }
    auto med = [](std::vector<double> v) {
      if (v.empty()) return 0.0;
      std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
      return v[v.size() / 2];
    };
    std::printf("%-6s %9" PRIu64 " %9.3f %9.3f %11.2f %11.2f %13.3f %12.2f\n", kRuleName[k], pred,
                pred ? static_cast<double>(tp) / static_cast<double>(pred) : 0.0,
                true_filled ? static_cast<double>(tp) / static_cast<double>(true_filled) : 0.0, med(err), med(aerr), med(qerr),
                true_filled ? static_cast<double>(pred) / static_cast<double>(true_filled) : 0.0);
  }
  std::printf("\ncancellations and size reductions at the touch: %" PRIu64 "\n", cancels_at_touch_);
  std::printf("relative queue position (quantity ahead / level; 0 = front, 1 = back), share per decile:\n  ");
  for (int i = 0; i < 10; ++i)
    std::printf("%5.1f%% ", cancels_at_touch_ ? 100.0 * static_cast<double>(cancel_hist_[i]) / static_cast<double>(cancels_at_touch_) : 0.0);
  std::printf("\n(uniform would be 10%% per decile)\n");
  const char* names[4] = {"1 order", "2-4 orders", "5-9 orders", "10+ orders"};
  std::printf("rank of the cancelled order among the level's orders (by count, front to back), by queue length:\n");
  for (int b = 1; b < 4; ++b) {
    std::printf("  %-11s n=%-7" PRIu64, names[b], bucket_n_[b]);
    for (int i = 0; i < 10; ++i)
      std::printf("%5.1f%% ", bucket_n_[b] ? 100.0 * static_cast<double>(rank_hist_[b][i]) / static_cast<double>(bucket_n_[b]) : 0.0);
    std::printf("\n");
  }
  std::printf("  (%" PRIu64 " cancels hit a level holding a single order)\n", bucket_n_[0]);
  std::printf("normalized rank of cancelled orders (0 front, 1 back; 0.5 if every order were equally likely):\n");
  for (int b = 1; b < 4; ++b) {
    const double fh = static_cast<double>(front_half_[b]), bh = static_cast<double>(back_half_[b]);
    std::printf("  %-11s mean %.3f, front half %.1f%% vs back half %.1f%%\n", names[b],
                bucket_n_[b] ? rank_sum_[b] / static_cast<double>(bucket_n_[b]) : 0.0,
                fh + bh > 0 ? 100 * fh / (fh + bh) : 0.0, fh + bh > 0 ? 100 * bh / (fh + bh) : 0.0);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  const tools::Args args(argc, argv);
  if (!args.has("in")) tools::Args::die("--in <file.exl3> is required");
  Study st(args.f64("probe-every", 5), args.f64("horizon", 300));
  l3::Reader rd(args.str("in", ""));
  const std::uint64_t max_records = args.u64("max-records", 0);
  std::uint64_t n = 0;
  while (const Rec* r = rd.next()) {
    if (max_records && ++n > max_records) break;
    st.on(*r);
  }
  if (st.gaps())
    std::printf("gaps in the archived feed: %" PRIu64 "; probes open at a gap dropped: %" PRIu64 "%s\n", st.gaps(),
                st.voided(), st.synced() ? "; resynchronized from a later snapshot" : "; ENDED OUT OF SYNC (no later snapshot)");
  st.finish(args.str("out-prefix", ""));
  return 0;
}
