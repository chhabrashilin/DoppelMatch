// exsim_l3replay: does the matching engine reproduce a real exchange's trades?
//
// Input: Coinbase BTC-USD order-by-order data (.exl3, from scripts/fetch_coinbase_l3.py + l3conv.py): every
// order the exchange received, every rest, cancel, modify and match, plus the exchange's L3 snapshot.
//
// Method. Two books run side by side:
//   * the TRUTH book (l3.hpp) applies Coinbase's own messages literally: no matching logic at all;
//   * the ENGINE (OrderBook) receives only the inputs (new orders, cancels, modifies) and does its own
//     price-time matching.
// Every order that arrives is an "episode": the engine predicts which resting orders it trades with, at
// what prices and sizes, and whether a remainder rests; the prediction is compared with Coinbase's actual
// match messages, exactly. On any divergence the engine's affected price levels are repaired from the truth
// book (a full rebuild is the fallback), so one unpredictable event cannot cascade: each prediction starts
// from the true pre-trade state. Periodically the whole engine book is compared with the truth book, order
// by order, in queue order.
//
// Inputs the public feed does not carry are inferred from the order's own lifecycle and counted, never
// hidden: time in force (an order that never rested was IOC/FOK), post-only rejections (an order that never
// rested and never traded), and the size of funds-denominated market orders (the filled quantity).
// Self-trade prevention is not labelled with accounts in an anonymous feed, but its effects are visible: an
// arrival that cancels or decrements another order without a trade reveals that both belong to one account
// and which STP mode the arrival used. Pass 1 infers these accounts; the engine then applies STP per order.
//
//   exsim_l3replay --in 2026-09-01.exl3 [--check-every 100000] [--examples 10] [--max-records N]
//                  [--no-stp] [--show-stp] [--drop-cancels N (fault injection)] [--trace]

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "args.hpp"
#include "exsim/l3.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/sinks.hpp"

using namespace exsim;
using l3::Rec;

namespace {

struct Life {  // per-order facts from a pre-pass over the file (inputs the feed does not state directly)
  bool opened = false;
  bool changed = false;
  Qty taker_filled = 0;
  Qty stp_cancelled = 0;  // size of the account's own orders cancelled inside this arrival's block
  Stp stp = Stp::None;  // self-trade prevention mode inferred for this order as an arrival (None: none observed)
};

// 2^24 ticks of $0.01 from $0: every realistic BTC-USD price ($0 to $167,772), so orders almost never fall
// outside the engine's band (a 268 MB ladder). Bots do park orders at a fraction of the price.
constexpr std::uint32_t kLevels = 1u << 24;
constexpr std::uint32_t kMaxOrders = 1u << 21;

// Accounts inferred from self-trade prevention. Coinbase processes an incoming order atomically, so any OTHER
// order that is cancelled or STP-decremented inside that order's processing block (between its `received`
// and its `open`/`done`) was removed because it belongs to the same account. Linking those orders gives
// the engine an owner for each, so its own self-trade prevention can reproduce what Coinbase did. Orders
// never linked get a unique owner (their own id), so they can never self-trade.
class Accounts {
 public:
  void link(OrderId a, OrderId b) {
    const OrderId ra = find(a), rb = find(b);
    if (ra != rb) parent_[std::max(ra, rb)] = std::min(ra, rb);
  }
  OrderId find(OrderId x) {
    auto it = parent_.find(x);
    if (it == parent_.end()) return x;
    const OrderId r = find(it->second);
    it->second = r;
    return r;
  }
  OwnerId owner(OrderId id) { return static_cast<OwnerId>(find(id) & 0xFFFF'FFFFu); }
  std::size_t linked() const { return parent_.size(); }

 private:
  std::unordered_map<OrderId, OrderId> parent_;
};

// Pass 1 over the file: lifecycle facts per order and self-trade-prevention accounts, which the replay then
// uses as inputs. Four kinds of evidence, all from Coinbase's own messages inside an arrival's block:
//   * another order is cancelled: cancel-resting ("co"), and the two orders share an account;
//   * ...and then the arrival itself is cancelled with size left, having traded nothing since the first such
//     cancel: cancel both
//     ("cb"), which the engine reproduces as cancel-incoming plus the resting order's own `done`;
//   * an STP `change` decrements an order: decrement-and-cancel ("dc");
//   * the arrival has traded, then is cancelled while the book still holds liquidity at its limit. A
//     size-limited IOC or market order does not stop there on its own, so the next order in queue priority
//     is its own: cancel-newest ("cn"). This one needs the book, so pass 1 keeps a truth book as well.
class Inference {
 public:
  std::unordered_map<OrderId, Life> life;
  Accounts accounts;
  std::uint64_t links = 0, cn_links = 0, cb_orders = 0;

  void on(const Rec& r) {
    if (r.kind == l3::SnapOrder) {
      if (synced_) return;
      if (!loading_) book_.clear(), loading_ = true;
      book_.add(r.id, r.side ? Side::Sell : Side::Buy, r.px, r.q, 0);
      return;
    }
    if (r.kind == l3::SnapEnd) {
      if (synced_) return;
      loading_ = false, synced_ = true;
      snap_seq_ = last_seq_ = r.seq;
      std::vector<Rec> early;
      early.swap(pre_);
      std::sort(early.begin(), early.end(), [](const Rec& a, const Rec& b) { return a.seq < b.seq; });
      for (const Rec& e : early)
        if (e.seq > snap_seq_) apply(e);
      return;
    }
    facts(r);  // lifecycle facts need no book: gather them from every message
    if (!synced_) {
      pre_.push_back(r);
      return;
    }
    if (r.seq <= snap_seq_) return;
    if (r.seq != last_seq_ + 1) {  // a gap: the book is unknown until the next snapshot
      synced_ = false;
      pre_.push_back(r);
      return;
    }
    apply(r);
  }

 private:
  void facts(const Rec& r) {
    if (r.kind == l3::Received) {
      block_ = r.id, block_side_ = r.side ? Side::Sell : Side::Buy, block_px_ = r.px;
      block_market_ = r.otype == 1, block_funds_ = (r.flags & l3::FundsOnly) != 0;
      block_filled_ = 0, block_cancelled_own_ = block_traded_after_cancel_ = false;
    }
    auto mark = [&](Stp m) {  // decrement-and-cancel wins over the others if both are seen in one block
      Stp& cur = life[block_].stp;
      if (cur != Stp::DecrementCancel) cur = m;
    };
    if (r.kind == l3::Open) {
      life[r.id].opened = true;
    } else if (r.kind == l3::Match) {
      life[r.id].taker_filled += r.q;
      if (r.id == block_) {
        block_filled_ += r.q;
        if (block_cancelled_own_) block_traded_after_cancel_ = true;
      }
    } else if (r.kind == l3::Change) {
      life[r.id].changed = true;
      if ((r.flags & l3::Stp) && block_ != 0) {
        if (r.id != block_) accounts.link(block_, r.id), ++links;  // the resting order was decremented
        mark(Stp::DecrementCancel);  // an STP decrement (of either order) only happens in decrement-and-cancel
      }
    } else if (r.kind == l3::Done && block_ != 0 && r.id != block_ && (r.flags & l3::Canceled)) {
      accounts.link(block_, r.id), ++links;  // another order cancelled inside this arrival's block
      mark(Stp::CancelResting);
      life[block_].stp_cancelled += r.q;
      block_cancelled_own_ = true;
    } else if (r.kind == l3::Done && r.id == block_ && (r.flags & l3::Canceled) && r.q > 0 &&
               life[block_].stp == Stp::CancelResting && block_cancelled_own_ && !block_traded_after_cancel_) {
      // It cancelled its own resting order, traded nothing after the first such cancel, and was itself cancelled
      // with size left: cancel both ("cb"; cancel-oldest would have kept matching). The engine stops at the own
      // order and cancels the arrival (cancel incoming); the resting order's own `done` then cancels it, so the
      // two books end identical. (Cancel-oldest with nothing left to match looks the same and ends the same.)
      life[block_].stp = Stp::CancelIncoming;
      ++cb_orders;
    } else if (r.kind == l3::Done && r.id == block_ && (r.flags & l3::Canceled) && block_filled_ > 0 && !block_funds_ &&
               r.q > 0 && synced_ && r.seq == last_seq_ + 1) {
      // Traded, then cancelled with size left. Is there still liquidity it could have taken?
      const Side opp = opposite(block_side_);
      if (const auto best = book_.best(opp)) {
        const bool crosses = block_market_ || (block_side_ == Side::Buy ? *best <= block_px_ : *best >= block_px_);
        const auto* q = book_.queue(opp, *best);
        if (crosses && q != nullptr && !q->empty()) {
          accounts.link(block_, q->front().id), ++links, ++cn_links;
          if (life[block_].stp == Stp::None) life[block_].stp = Stp::CancelIncoming;
        }
      }
    }
    if ((r.kind == l3::Open || r.kind == l3::Done) && r.id == block_) block_ = 0;  // block ends
  }

  void apply(const Rec& r) {
    last_seq_ = r.seq;
    const Side side = r.side ? Side::Sell : Side::Buy;
    switch (r.kind) {
      case l3::Open: book_.add(r.id, side, r.px, r.q, r.seq); break;
      case l3::Match: book_.fill(r.id2, r.q); break;
      case l3::Done: book_.remove(r.id); break;
      case l3::Change:
        if (!(r.flags & l3::FundsChange)) {
          Side s;
          Price old_px;
          if (book_.find(r.id, &s, &old_px)) book_.change(r.id, r.px ? r.px : old_px, r.q, r.seq);
        }
        break;
      default: break;
    }
  }

  l3::TruthBook book_;
  bool loading_ = false, synced_ = false;
  std::uint64_t snap_seq_ = 0, last_seq_ = 0;
  std::vector<Rec> pre_;
  OrderId block_ = 0;  // the arrival whose processing block we are in (0: none)
  Side block_side_ = Side::Buy;
  Price block_px_ = 0;
  bool block_market_ = false, block_funds_ = false;
  Qty block_filled_ = 0;
  bool block_cancelled_own_ = false, block_traded_after_cancel_ = false;
};

struct Trade {
  OrderId maker;
  Price px;
  Qty qty;
  bool operator==(const Trade&) const = default;
};

struct Stats {
  std::uint64_t records = 0, received = 0, received_market = 0, funds_derived = 0, post_only_inferred = 0;
  std::uint64_t ioc_inferred = 0, out_of_band = 0, cancels = 0, modifies = 0, stp_changes = 0;
  std::uint64_t episodes = 0, trading_episodes = 0, exact = 0, mismatched = 0, mismatched_stp = 0;
  std::uint64_t cb_matches = 0, matched_exact = 0, rest_checked = 0, rest_mismatch = 0;
  std::uint64_t book_checks = 0, book_check_failures = 0, rebuilds = 0, repairs = 0, gaps = 0, truth_errors = 0;
  std::uint64_t resyncs = 0, abandoned = 0, unreplayed = 0, unmatched_fill_done = 0, unmatched_fill_qty = 0;
  std::uint64_t orphan_matches = 0, clamped = 0, fill_modifies = 0, modify_out_of_band = 0, stp_orders = 0;
};

class Replay {
 public:
  Replay(std::unordered_map<OrderId, Life> life, Accounts accounts, bool stp, std::uint64_t check_every, int examples)
      : life_(std::move(life)), accounts_(std::move(accounts)), stp_(stp), check_every_(check_every), examples_(examples) {}

  void run(l3::Reader& rd, std::uint64_t max_records) {
    const auto t0 = std::chrono::steady_clock::now();
    while (const Rec* r = rd.next()) {
      if (max_records && st_.records >= max_records) break;
      ++st_.records;
      on(*r);
      if (st_.records % 2'000'000 == 0)
        std::printf("  ... %" PRIu64 " records, %" PRIu64 " episodes, %" PRIu64 " diverged, %" PRIu64 " repairs, %" PRIu64 " rebuilds\n",
                    st_.records, st_.episodes, st_.mismatched, st_.repairs, st_.rebuilds);
    }
    close_episode();
    wall_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  }

  void report() const {
    const auto pct = [](std::uint64_t a, std::uint64_t b) { return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; };
    std::printf("records %" PRIu64 " in %.1f s; sequence gaps %" PRIu64 "; truth-book inconsistencies %" PRIu64
                "; matches with no known taker %" PRIu64 "\n",
                st_.records, wall_, st_.gaps, st_.truth_errors, st_.orphan_matches);
    if (st_.gaps != 0)
      std::printf("gaps in the archived feed: %" PRIu64 ", recovered from a later snapshot: %" PRIu64
                  "; episodes open at a gap (not scored): %" PRIu64 "; messages covered by a snapshot instead: %" PRIu64 "%s\n",
                  st_.gaps, st_.resyncs, st_.abandoned, st_.unreplayed,
                  synced_ ? "" : "; ENDED OUT OF SYNC (no snapshot after the last gap)");
    std::printf("orders received %" PRIu64 " (market %" PRIu64 ", funds-denominated %" PRIu64 "), cancels %" PRIu64
                ", modifies %" PRIu64 " (%" PRIu64 " filled completely), STP changes %" PRIu64 "\n",
                st_.received, st_.received_market, st_.funds_derived, st_.cancels, st_.modifies, st_.fill_modifies, st_.stp_changes);
    std::printf("limit prices beyond the engine's band: %" PRIu64 " arrivals or fill-modifies clamped to the band edge (never rest), %" PRIu64
                " rest out of band (not replayable), %" PRIu64 " modified out of the band\n",
                st_.clamped, st_.out_of_band, st_.modify_out_of_band);
    std::printf("reported filled with part of the size never matched (exchange inconsistency): %" PRIu64
                " orders, %" PRIu64 " lots\n",
                st_.unmatched_fill_done, st_.unmatched_fill_qty);
    std::printf("inferred from lifecycle: %" PRIu64 " IOC/FOK, %" PRIu64 " post-only/zero-fill, %" PRIu64
                " arrivals with a self-trade-prevention mode\n",
                st_.ioc_inferred, st_.post_only_inferred, st_.stp_orders);
    std::printf("\nepisodes (arrivals + price/size modifies): %" PRIu64 "; with trades on either side: %" PRIu64 "\n",
                st_.episodes, st_.trading_episodes);
    std::printf("  reproduced exactly (same makers, prices, sizes, in order; same resting remainder): %" PRIu64 " (%.4f%%)\n",
                st_.exact, pct(st_.exact, st_.episodes));
    std::printf("  diverged: %" PRIu64 " (%" PRIu64 " involving self-trade prevention, %" PRIu64 " other)\n", st_.mismatched,
                st_.mismatched_stp, st_.mismatched - st_.mismatched_stp);
    std::printf("Coinbase matches: %" PRIu64 "; predicted exactly by the engine: %" PRIu64 " (%.4f%%)\n", st_.cb_matches,
                st_.matched_exact, pct(st_.matched_exact, st_.cb_matches));
    std::printf("resting remainders checked against Coinbase's 'open': %" PRIu64 ", mismatched %" PRIu64 "\n", st_.rest_checked,
                st_.rest_mismatch);
    std::printf("full-book comparisons (engine vs truth, every order in queue order): %" PRIu64 ", failed %" PRIu64
                "; local level repairs %" PRIu64 ", full rebuilds from truth %" PRIu64 "\n",
                st_.book_checks, st_.book_check_failures, st_.repairs, st_.rebuilds);
  }

  bool ok() const { return st_.mismatched == st_.mismatched_stp && st_.book_check_failures == 0 && synced_; }

 private:
  // ---------------- engine plumbing ----------------
  struct Sink {
    std::vector<Event> ev;
    void on_event(const Event& e) { ev.push_back(e); }
  };

  void make_engine(Price mid) {
    BookConfig cfg;
    cfg.min_price = 0;
    (void)mid;
    cfg.num_levels = kLevels;
    cfg.max_orders = kMaxOrders;
    cfg.stp = stp_ ? Stp::CancelResting : Stp::None;
    min_px_ = cfg.min_price;
    engine_ = std::make_unique<OrderBook<AosStore>>(0, cfg);
  }
  bool in_band(Price px) const { return px >= min_px_ && px < min_px_ + static_cast<Price>(kLevels); }

  void rebuild_from_truth() {
    ++st_.rebuilds;
    std::vector<OrderId> live;
    for (Side s : {Side::Buy, Side::Sell}) engine_->for_each_order(s, [&](Price, OrderId id, Qty) { live.push_back(id); });
    Sink sink;
    for (OrderId id : live) {
      Command c{};
      c.type = MsgType::Cancel, c.order_id = id;
      engine_->cancel(c, sink);
    }
    for (Side s : {Side::Buy, Side::Sell}) {
      truth_.for_each(s, [&](Price px, const l3::TruthBook::Order& o) {
        if (o.qty == 0 || !in_band(px)) return;
        Command c{};
        c.type = MsgType::NewOrder, c.order_id = o.id, c.side = s, c.price = px, c.qty = o.qty, c.owner = accounts_.owner(o.id);
        engine_->add(c, sink);
      });
    }
  }

  // Resyncs the given price levels (both sides) from the truth book. Returns false if re-adding an order
  // traded, which means the engine was inconsistent somewhere else too: the caller then rebuilds fully.
  bool repair_levels(const std::vector<Price>& prices) {
    ++st_.repairs;
    Sink sink;
    for (Price px : prices) {
      std::vector<OrderId> ids;
      engine_->for_each_order_at(px, [&](OrderId id, Qty, Side) { ids.push_back(id); });
      for (OrderId id : ids) {
        Command c{};
        c.type = MsgType::Cancel, c.order_id = id;
        engine_->cancel(c, sink);
      }
    }
    for (Price px : prices) {
      if (!in_band(px)) continue;
      for (Side s : {Side::Buy, Side::Sell}) {
        const auto* q = truth_.queue(s, px);
        if (q == nullptr) continue;
        for (const auto& o : *q) {
          if (o.qty == 0) continue;
          if (engine_->contains(o.id)) {  // it rests at another price in the engine: move it
            Command c{};
            c.type = MsgType::Cancel, c.order_id = o.id;
            engine_->cancel(c, sink);
          }
          Command c{};
          c.type = MsgType::NewOrder, c.order_id = o.id, c.side = s, c.price = px, c.qty = o.qty, c.owner = accounts_.owner(o.id);
          engine_->add(c, sink);
        }
      }
    }
    for (const Event& e : sink.ev)
      if (e.type == EventType::Trade) return false;
    return true;
  }

  bool books_equal() const {
    for (Side s : {Side::Buy, Side::Sell}) {
      std::vector<std::tuple<Price, OrderId, Qty>> a, b;
      engine_->for_each_order(s, [&](Price p, OrderId id, Qty q) { a.emplace_back(p, id, q); });
      truth_.for_each(s, [&](Price p, const l3::TruthBook::Order& o) {
        if (o.qty != 0 && in_band(p)) b.emplace_back(p, o.id, o.qty);
      });
      if (a != b) return false;
    }
    return true;
  }

  // ---------------- episodes ----------------
  void open_episode(OrderId taker, std::vector<Event>&& ev, bool expect_rest_known) {
    close_episode();
    ep_active_ = true;
    ep_taker_ = taker;
    ep_pred_.clear();
    ep_actual_.clear();
    ep_stp_ = false;
    ep_taker_rested_ = false;
    ep_rest_known_ = expect_rest_known;
    for (const Event& e : ev)
      if (e.type == EventType::Trade) ep_pred_.push_back(Trade{e.maker_id, e.price, e.qty});
    ++st_.episodes;
  }

  // --trace: the lifecycle facts of the taker and of every maker involved, and the messages around the episode.
  void print_trace() {
    auto facts = [&](const char* role, OrderId id) {
      const auto it = life_.find(id);
      const Life l = it == life_.end() ? Life{} : it->second;
      Side s = Side::Buy;
      Price px = 0;
      const auto* o = truth_.find(id, &s, &px);
      std::printf("        %s %" PRIu64 ": opened=%d changed=%d taker_filled=%" PRIu64 " stp=%d account=%u resting=%s\n",
                  role, id, l.opened, l.changed, l.taker_filled, static_cast<int>(l.stp), accounts_.owner(id),
                  o ? (std::to_string(o->qty) + " @ " + std::to_string(px)).c_str() : "no");
    };
    facts("taker", ep_taker_);
    for (const Trade& t : ep_pred_) facts("engine maker", t.maker);
    for (const Trade& t : ep_actual_) facts("coinbase maker", t.maker);
    static constexpr const char* kNames[] = {"", "SNAP", "SNAPEND", "RECV", "OPEN", "DONE", "MATCH", "CHANGE"};
    for (const Rec& x : ring_)
      std::printf("        %-7s seq=%" PRIu64 " id=%" PRIu64 " id2=%" PRIu64 " %c px=%" PRId64 " px2=%" PRId64 " q=%" PRIu64
                  " q2=%" PRIu64 " otype=%u flags=%u\n",
                  x.kind < 8 ? kNames[x.kind] : "?", x.seq, x.id, x.id2, x.side ? 'S' : 'B', x.px, x.px2, x.q, x.q2,
                  static_cast<unsigned>(x.otype), static_cast<unsigned>(x.flags));
  }

  std::deque<Rec> ring_;  // recent messages, kept only with --trace

  // A gap in the feed: the open episode may be missing messages, so it is not scored at all.
  void abandon_episode() {
    if (!ep_active_) return;
    ep_active_ = false;
    --st_.episodes;
    ++st_.abandoned;
  }

  void close_episode() {
    if (!ep_active_) return;
    ep_active_ = false;
    if (!ep_pred_.empty() || !ep_actual_.empty()) ++st_.trading_episodes;
    st_.cb_matches += ep_actual_.size();
    bool same = ep_pred_ == ep_actual_;
    if (same) {
      st_.matched_exact += ep_actual_.size();
    } else {
      // count the longest common prefix as matched (the engine agreed up to the first divergence)
      std::size_t k = 0;
      while (k < ep_pred_.size() && k < ep_actual_.size() && ep_pred_[k] == ep_actual_[k]) ++k;
      st_.matched_exact += k;
    }
    if (same) {
      ++st_.exact;
      return;
    }
    ++st_.mismatched;
    if (ep_stp_) ++st_.mismatched_stp;
    for (const Trade& t : ep_pred_) if (t.px >= 0) repair_px_.push_back(t.px);
    for (const Trade& t : ep_actual_) if (t.px >= 0) repair_px_.push_back(t.px);
    if (ep_taker_px_ != 0) repair_px_.push_back(ep_taker_px_);
    if (shown_ < examples_ && (!ep_stp_ || show_stp_)) {
      ++shown_;
      std::printf("  divergence%s at seq %" PRIu64 ", taker %" PRIu64 ": engine %zu trades, Coinbase %zu\n",
                  ep_stp_ ? " (self-trade prevention)" : "", ep_seq_, ep_taker_, ep_pred_.size(), ep_actual_.size());
      for (std::size_t i = 0; i < std::max(ep_pred_.size(), ep_actual_.size()) && i < 6; ++i) {
        auto show = [](const std::vector<Trade>& v, std::size_t j) {
          if (j >= v.size()) return std::string("-");
          char b[96];
          std::snprintf(b, sizeof b, "maker %" PRIu64 " %" PRId64 " x %" PRIu64, v[j].maker, v[j].px, v[j].qty);
          return std::string(b);
        };
        std::printf("      engine %-40s coinbase %s\n", show(ep_pred_, i).c_str(), show(ep_actual_, i).c_str());
      }
      if (trace_) print_trace();
    }
    need_rebuild_ = true;
  }

  // Episode membership: matches with this taker, the maker 'done's, and the taker's own open/done.
  bool in_episode(const Rec& r) const {
    if (!ep_active_) return false;
    // Once the taker has rested its arrival is over: a later match with it as taker is a modify crossing the
    // spread (its matches precede the `change`), not part of this episode.
    if (r.kind == l3::Match) return r.id == ep_taker_ && !ep_taker_rested_;
    if (r.kind == l3::Done || r.kind == l3::Open) return true;  // closed when the next arrival starts
    if (r.kind == l3::Change) return (r.flags & l3::Stp) != 0;
    return false;
  }

  // ---------------- message handling ----------------
  void on(const Rec& r) {
    if (trace_ && r.kind > l3::SnapEnd) {
      ring_.push_back(r);
      if (ring_.size() > 40) ring_.pop_front();
    }
    // Snapshots. Tardis records one when its capture connection starts (00:00 UTC) and again whenever it
    // reconnects. While in sync the truth book already reflects a later snapshot, which is skipped; after a
    // gap the next snapshot is the only way back.
    if (r.kind == l3::SnapOrder) {
      if (synced_) return;
      if (!loading_snap_) { truth_.clear(); loading_snap_ = true; }
      truth_.add(r.id, r.side ? Side::Sell : Side::Buy, r.px, r.q, r.ts);
      return;
    }
    if (r.kind == l3::SnapEnd) {
      if (synced_) return;
      loading_snap_ = false;
      synced_ = true;
      const bool resync = snap_seq_ != 0;
      snap_seq_ = last_seq_ = r.seq;
      const Price mid = (*truth_.best(Side::Buy) + *truth_.best(Side::Sell)) / 2;
      make_engine(mid);
      rebuild_from_truth();
      --st_.rebuilds;  // loading a snapshot is not a rebuild
      if (resync) ++st_.resyncs;
      std::printf("%s from exchange snapshot seq %" PRIu64 ": %zu orders (engine band %" PRId64 "..%" PRId64 " ticks)\n",
                  resync ? "resynchronized" : "seeded", r.seq, truth_.size(), min_px_, min_px_ + static_cast<Price>(kLevels));
      // The snapshot is fetched while the stream is already running, so messages newer than it can appear
      // earlier in the file. Replay the buffered ones that the snapshot does not already reflect.
      std::vector<Rec> early;
      early.swap(pre_snapshot_);
      std::sort(early.begin(), early.end(), [](const Rec& a, const Rec& b) { return a.seq < b.seq; });
      std::size_t used = 0;
      for (const Rec& e : early)
        if (e.seq > snap_seq_) ++used, on(e);
      st_.unreplayed += early.size() - used;
      std::printf("replayed %zu messages that arrived before the snapshot but are newer than it\n", used);
      return;
    }
    if (!synced_) {  // before a snapshot: keep, it may be newer than the snapshot's sequence
      pre_snapshot_.push_back(r);
      return;
    }
    if (r.seq <= snap_seq_) return;  // already reflected in the snapshot
    if (r.seq != last_seq_ + 1) {
      // Messages are missing, so neither book can be trusted until the next snapshot.
      ++st_.gaps;
      std::printf("sequence gap: %" PRIu64 " -> %" PRIu64 " (%" PRIu64 " messages missing from the archive); waiting for "
                  "the next snapshot\n", last_seq_, r.seq, r.seq - last_seq_ - 1);
      abandon_episode();
      modify_fills_.clear();
      need_rebuild_ = false;
      repair_px_.clear();
      synced_ = false;
      pre_snapshot_.push_back(r);
      return;
    }
    last_seq_ = r.seq;

    if (!in_episode(r)) close_episode();
    if (need_rebuild_ && (r.kind == l3::Received || (r.kind == l3::Change && !(r.flags & l3::Stp)))) {
      need_rebuild_ = false;
      std::sort(repair_px_.begin(), repair_px_.end());
      repair_px_.erase(std::unique(repair_px_.begin(), repair_px_.end()), repair_px_.end());
      if (!repair_levels(repair_px_)) rebuild_from_truth();
      repair_px_.clear();
    }
    // Whole-book comparison, between episodes and before the next arrival touches either book.
    if (pending_check_ && (r.kind == l3::Received || (r.kind == l3::Change && !(r.flags & l3::Stp)))) {
      pending_check_ = false;
      full_check();
    }

    const Side side = r.side ? Side::Sell : Side::Buy;
    switch (r.kind) {
      case l3::Received: on_received(r, side); break;
      case l3::Open:
        truth_.add(r.id, side, r.px, r.q, r.ts);
        if (ep_active_ && r.id == ep_taker_) ep_taker_rested_ = true;
        if (ep_active_ && r.id == ep_taker_ && in_band(r.px)) {
          ++st_.rest_checked;
          const auto v = engine_->find_order(r.id);
          if (!v || v->qty != r.q || v->price != r.px) {
            ++st_.rest_mismatch;
            if (ep_pred_ == ep_actual_) {  // trades agreed but the remainder did not: still a divergence
              ep_actual_.push_back(Trade{0, -1, 0});
            }
          }
        }
        break;
      case l3::Done: {
        const bool fill_modify = modify_fills_.count(r.id) != 0;
        if (fill_modify) {  // a modify that filled the order completely: no `change` is sent
          Price old_px = 0;
          Side mside = Side::Buy;
          truth_.find(r.id, &mside, &old_px);
          ++st_.modifies;
          ++st_.fill_modifies;
          modify_episode(r.id, mside, r.px, old_px, 0, r.seq);
        }
        const bool was_resting = truth_.contains(r.id);
        // Coinbase occasionally reports a resting order "filled" although its published matches leave part
        // of it unaccounted for (the raw feed has no gap: the size simply disappears). The exchange's word is
        // final, so the engine drops the remainder too; each case is counted.
        if (!(r.flags & l3::Canceled) && was_resting && !fill_modify) {
          Side fs = Side::Buy;
          Price fpx = 0;
          if (const auto* o = truth_.find(r.id, &fs, &fpx); o != nullptr && o->qty > 0) {
            ++st_.unmatched_fill_done;
            st_.unmatched_fill_qty += o->qty;
            if (trace_)
              std::printf("  filled with %" PRIu64 " unmatched: order %" PRIu64 " %s @ %" PRId64 " (done seq %" PRIu64
                          ", done px %" PRId64 ")\n",
                          o->qty, r.id, fs == Side::Buy ? "buy" : "sell", fpx, r.seq, r.px);
            Command c{};
            c.type = MsgType::Cancel, c.order_id = r.id;
            Sink sink;
            engine_->cancel(c, sink);
          }
        }
        truth_.remove(r.id);
        if ((r.flags & l3::Canceled) && was_resting) {
          ++st_.cancels;
          if (ep_active_ && r.id != ep_taker_ && !ep_actual_.empty()) ep_stp_ = true;  // a maker vanished mid-sweep
          // Fault injection (--drop-cancels N): the engine never hears of every Nth cancel. The ghost orders
          // this leaves must surface as divergences; if they did not, the comparison could not fail.
          if (drop_cancel_every_ == 0 || ++cancel_count_ % drop_cancel_every_ != 0) {
            Command c{};
            c.type = MsgType::Cancel, c.order_id = r.id;
            Sink sink;
            engine_->cancel(c, sink);
          }
        }
        break;
      }
      case l3::Match:
        if (!truth_.fill(r.id2, r.q)) ++st_.truth_errors;
        if (ep_active_ && r.id == ep_taker_) {
          ep_actual_.push_back(Trade{r.id2, r.px, r.q});
        } else if (truth_.contains(r.id)) {
          // The taker already rests in the book: this is an order being modified across the spread. Coinbase
          // reports the resulting matches BEFORE the `change` message, whose new size is the post-trade
          // remainder. Hold the matches until that `change` arrives (on_change).
          modify_fills_[r.id].push_back(Trade{r.id2, r.px, r.q});
        } else {
          ++st_.orphan_matches;
        }
        break;
      case l3::Change: on_change(r); break;
      default: break;
    }
    if (check_every_ && st_.records - last_check_ >= check_every_ && r.kind == l3::Received) {
      last_check_ = st_.records;
      pending_check_ = true;  // run it when this arrival's episode has fully played out
    }
  }

  void on_received(const Rec& r, Side side) {
    ++st_.received;
    const Life info = life_.count(r.id) ? life_.at(r.id) : Life{};
    Command c{};
    c.type = MsgType::NewOrder, c.order_id = r.id, c.side = side, c.owner = accounts_.owner(r.id);
    if (info.stp != Stp::None) c.flags |= stp_flag(info.stp), ++st_.stp_orders;
    if (r.otype == 1) {
      ++st_.received_market;
      c.ord_type = OrdType::Market;
      c.qty = r.q;
      if (r.flags & l3::FundsOnly) {  // notional-denominated: replay with the quantity Coinbase filled
        ++st_.funds_derived;
        c.qty = info.taker_filled;
        // Under decrement-and-cancel Coinbase decrements a funds order's FUNDS, which the filled quantity already
        // reflects; the engine decrements the order's size by each own order it cancels, so add those back.
        if (info.stp == Stp::DecrementCancel) c.qty += info.stp_cancelled;
      }
    } else {
      c.ord_type = OrdType::Limit;
      c.price = r.px;
      c.qty = r.q;
      if (!in_band(r.px)) {
        if (info.opened) {  // it rests outside the band: the engine cannot hold it
          ++st_.out_of_band;
          return;
        }
        // A limit far through the market (e.g. a sell at $0.01) that never rests: clamping it to the band
        // edge matches exactly the same orders, because nothing rests beyond the band.
        ++st_.clamped;
        c.price = r.px < min_px_ ? min_px_ : min_px_ + static_cast<Price>(kLevels) - 1;
      }
      if (!info.opened) {
        if (info.taker_filled == 0 && !info.changed) {
          c.flags |= kFlagPostOnly, c.tif = Tif::Ioc;  // never rested, never traded
          ++st_.post_only_inferred;
        } else {
          c.tif = Tif::Ioc;
          ++st_.ioc_inferred;
        }
      }
    }
    if (c.qty == 0) {  // a funds market order that filled nothing: no trades either way
      open_episode(r.id, {}, false);
      ep_seq_ = r.seq;
      return;
    }
    Sink sink;
    engine_->add(c, sink);
    open_episode(r.id, std::move(sink.ev), true);
    ep_seq_ = r.seq;
    ep_taker_px_ = c.ord_type == OrdType::Limit ? c.price : 0;
  }

  void on_change(const Rec& r) {
    if (r.flags & l3::FundsChange) return;  // funds of an in-flight market order: nothing rests
    Side side;
    Price old_px;
    const auto* o = truth_.find(r.id, &side, &old_px);
    if (o == nullptr) return;
    const Price new_px = r.px ? r.px : old_px;
    truth_.change(r.id, new_px, r.q, r.ts);
    if (r.flags & l3::Stp) {
      ++st_.stp_changes;
      if (ep_active_) ep_stp_ = true;
      Command c{};
      c.type = MsgType::Modify, c.order_id = r.id, c.price = new_px, c.qty = r.q;
      Sink sink;
      engine_->modify(c, sink);
      return;
    }
    ++st_.modifies;
    modify_episode(r.id, side, new_px, old_px, r.q, r.seq);
  }

  // A modify, compared against the matches Coinbase reported for it (which precede the `change`, or the
  // `done` when the modify filled the order completely). `remainder` is the post-trade size.
  void modify_episode(OrderId id, Side side, Price new_px, Price old_px, Qty remainder, std::uint64_t seq) {
    std::vector<Trade> fills;
    if (auto it = modify_fills_.find(id); it != modify_fills_.end()) {
      fills = std::move(it->second);
      modify_fills_.erase(it);
    }
    Qty filled = 0;
    for (const Trade& t : fills) filled += t.qty;
    Sink sink;
    if (!in_band(new_px) && remainder == 0 && filled > 0) {
      // Modified far through the market and filled completely (a fat-fingered edit to $783,560 was seen):
      // as with arrivals, the band edge matches exactly the same orders, because nothing rests beyond it.
      ++st_.clamped;
      new_px = new_px < min_px_ ? min_px_ : min_px_ + static_cast<Price>(kLevels) - 1;
    }
    if (!in_band(new_px)) {  // moved beyond the band: it leaves the engine (it cannot trade there)
      ++st_.modify_out_of_band;
      Command k{};
      k.type = MsgType::Cancel, k.order_id = id;
      engine_->cancel(k, sink);
      sink.ev.clear();
    } else if (!engine_->contains(id)) {  // moved into the band: a price change goes to the back anyway
      Command c{};
      c.type = MsgType::NewOrder, c.order_id = id, c.side = side, c.price = new_px, c.qty = remainder + filled,
      c.owner = accounts_.owner(id);
      engine_->add(c, sink);
    } else {
      Command c{};
      c.type = MsgType::Modify, c.order_id = id, c.price = new_px, c.qty = remainder + filled;
      engine_->modify(c, sink);
    }
    open_episode(id, std::move(sink.ev), true);
    ep_actual_ = std::move(fills);
    if (remainder == 0) {  // fully filled by the modify: nothing may rest
      Command k{};
      k.type = MsgType::Cancel, k.order_id = id;
      Sink ks;
      engine_->cancel(k, ks);
    }
    ep_seq_ = seq;
    ep_taker_px_ = new_px;
    repair_px_.push_back(old_px);  // only used if this episode diverges
  }

  void full_check() {
    ++st_.book_checks;
    if (!books_equal()) {
      ++st_.book_check_failures;
      if (diag_shown_ < examples_) {
        ++diag_shown_;
        describe_first_difference();
      }
      rebuild_from_truth();
    }
  }

  // Prints the first order at which the engine and truth books disagree (either side), for diagnosis.
  void describe_first_difference() const {
    for (Side s : {Side::Buy, Side::Sell}) {
      std::vector<std::tuple<Price, OrderId, Qty>> a, b;
      engine_->for_each_order(s, [&](Price p, OrderId id, Qty q) { a.emplace_back(p, id, q); });
      truth_.for_each(s, [&](Price p, const l3::TruthBook::Order& o) {
        if (o.qty != 0 && in_band(p)) b.emplace_back(p, o.id, o.qty);
      });
      for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        if (i < a.size() && i < b.size() && a[i] == b[i]) continue;
        auto show = [](const std::vector<std::tuple<Price, OrderId, Qty>>& v, std::size_t j) {
          if (j >= v.size()) return std::string("-");
          char buf[96];
          std::snprintf(buf, sizeof buf, "px %" PRId64 " id %" PRIu64 " qty %" PRIu64, std::get<0>(v[j]), std::get<1>(v[j]),
                        std::get<2>(v[j]));
          return std::string(buf);
        };
        std::printf("  book check failed at seq %" PRIu64 ", %s side, position %zu: engine %s | truth %s\n", last_seq_,
                    s == Side::Buy ? "bid" : "ask", i, show(a, i).c_str(), show(b, i).c_str());
        return;
      }
    }
  }

  std::unordered_map<OrderId, Life> life_;
  Accounts accounts_;
  bool stp_;
  std::uint64_t check_every_;
  int examples_, shown_ = 0, diag_shown_ = 0;

 public:
  bool show_stp_ = false, trace_ = false;
  std::uint64_t drop_cancel_every_ = 0, cancel_count_ = 0;

 private:
  l3::TruthBook truth_;
  std::unique_ptr<OrderBook<AosStore>> engine_;
  Price min_px_ = 0;
  bool loading_snap_ = false, synced_ = false, need_rebuild_ = false;
  std::uint64_t snap_seq_ = 0, last_seq_ = 0;
  bool ep_active_ = false, ep_stp_ = false, ep_rest_known_ = false, ep_taker_rested_ = false;
  OrderId ep_taker_ = 0;
  std::uint64_t ep_seq_ = 0;
  std::vector<Trade> ep_pred_, ep_actual_;
  std::vector<Rec> pre_snapshot_;
  std::vector<Price> repair_px_;
  std::unordered_map<OrderId, std::vector<Trade>> modify_fills_;
  std::uint64_t last_check_ = 0;
  bool pending_check_ = false;
  Price ep_taker_px_ = 0;
  Stats st_;
  double wall_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);  // progress lines appear as they happen, even through a pipe
  const tools::Args args(argc, argv);
  if (!args.has("in")) tools::Args::die("--in <file.exl3> is required");
  const std::string path = args.str("in", "");
  const std::uint64_t max_records = args.u64("max-records", 0);

  // Pass 1: per-order lifecycle facts (did it ever rest? how much did it take as a taker? was it modified?),
  // and accounts inferred from self-trade prevention inside each arrival's processing block.
  Inference inf;
  {
    l3::Reader rd(path);
    std::uint64_t n = 0;
    while (const Rec* r = rd.next()) {
      if (max_records && ++n > max_records) break;
      inf.on(*r);
    }
  }
  const bool stp = !args.has("no-stp");
  std::printf("self-trade prevention: %" PRIu64 " links inferred between %zu orders (%" PRIu64
              " of them from cancel-newest: an arrival stopped with crossing liquidity left); %" PRIu64
              " arrivals cancelled together with their own resting order (cancel both); engine STP %s\n",
              inf.links, inf.accounts.linked(), inf.cn_links, inf.cb_orders, stp ? "on" : "off");
  // Pass 2: replay.
  l3::Reader rd(path);
  Replay rp(std::move(inf.life), std::move(inf.accounts), stp, args.u64("check-every", 100000),
            static_cast<int>(args.u64("examples", 10)));
  rp.show_stp_ = args.has("show-stp");
  rp.drop_cancel_every_ = args.u64("drop-cancels", 0);
  rp.trace_ = args.has("trace");
  rp.run(rd, max_records);
  rp.report();
  return rp.ok() ? 0 : 1;
}
