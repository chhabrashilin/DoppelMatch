// Price-time priority limit order book for one symbol.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "exsim/common.hpp"
#include "exsim/memory.hpp"
#include "exsim/order_index.hpp"
#include "exsim/order_store.hpp"
#include "exsim/price_bitmap.hpp"

namespace exsim {

// Design, in the order the hot path touches it:
//
//  * Price ladder. Price levels form a dense array indexed by (price - min_price), so finding a
//    level is an array index: no tree walk, no hash. A level is 16 bytes {agg qty, FIFO head, FIFO
//    tail}, four to a cache line. Bids and asks share the ladder, since an uncrossed book never has
//    both sides resting at one price.
//  * Best price. best_bid_ / best_ask_ are cached level indices. An empty side is -1 (bids) or
//    num_levels (asks), so "does the incoming order cross?" is one signed compare with no separate
//    emptiness check. When a best level empties, a two-level PriceBitmap finds the next one.
//  * FIFO per level. An intrusive doubly linked list of store slots. The head's `prev` is never read,
//    so popping the head during a sweep writes only the level's head field.
//  * Orders. Slots in a preallocated Store (a layout policy; see order_store.hpp), reached by id via
//    a flat OrderIndex (hash policy: see order_index.hpp).
//  * Events. Delivered to a Sink template parameter. There is no virtual dispatch, and a sink that
//    ignores an event type costs nothing.
//
// Nothing on any path through add/cancel/modify allocates. tests/test_alloc.cpp enforces this.
template <class Store, class IndexHash = LocalityHash>
class OrderBook {
 public:
  using store_type = Store;

  struct Level {
    std::uint64_t qty;
    std::uint32_t head;
    std::uint32_t tail;
  };
  static_assert(sizeof(Level) == 16);

  OrderBook(SymbolId symbol, const BookConfig& cfg)
      : symbol_(symbol),
        min_price_(cfg.min_price),
        n_(static_cast<std::int32_t>(cfg.num_levels)),
        stp_(cfg.stp),
        best_bid_(-1),
        best_ask_(static_cast<std::int32_t>(cfg.num_levels)),
        levels_(cfg.num_levels),
        bid_bits_(cfg.num_levels),
        ask_bits_(cfg.num_levels),
        ids_(cfg.max_orders),
        store_(cfg.max_orders) {
    if (cfg.max_orders == 0 || cfg.max_orders >= kNil) throw std::invalid_argument("OrderBook: bad max_orders");
    for (std::int32_t i = 0; i < n_; ++i) levels_[static_cast<std::size_t>(i)] = Level{0, kNil, kNil};
  }

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  template <class Sink>
  EXSIM_ALWAYS_INLINE void add(const Command& c, Sink& sink) noexcept {
    if (c.side == Side::Buy)
      add_side<Side::Buy>(c, sink);
    else
      add_side<Side::Sell>(c, sink);
  }

  template <class Sink>
  EXSIM_ALWAYS_INLINE void cancel(const Command& c, Sink& sink) noexcept {
    const std::uint32_t s = ids_.erase(c.order_id, key_of());
    if (EXSIM_UNLIKELY(s == kNil)) return sink.on_event(rejected_event(c, Reason::UnknownOrder));
    const Side side = store_.side(s);
    const Qty leaves = store_.qty(s);
    const Price px = min_price_ + store_.level(s);
    unlink(s);
    store_.release(s);
    sink.on_event(canceled_event(symbol_, c.order_id, side, px, leaves, Reason::UserCanceled));
  }

  // Cancel/replace semantics. Reducing quantity at the same price amends in place and keeps queue
  // priority. A price change or a size increase re-enters the order at the back of the queue, and
  // the re-entered order may match immediately.
  template <class Sink>
  EXSIM_ALWAYS_INLINE void modify(const Command& c, Sink& sink) noexcept {
    if (EXSIM_UNLIKELY(c.qty == 0)) return sink.on_event(rejected_event(c, Reason::InvalidQty));
    const std::uint32_t s = ids_.find(c.order_id, key_of());
    if (EXSIM_UNLIKELY(s == kNil)) return sink.on_event(rejected_event(c, Reason::UnknownOrder));
    const Price rel = c.price - min_price_;
    if (EXSIM_UNLIKELY(rel < 0 || rel >= n_)) return sink.on_event(rejected_event(c, Reason::InvalidPrice));
    const auto ix = static_cast<std::int32_t>(rel);
    const Side side = store_.side(s);

    if (ix == static_cast<std::int32_t>(store_.level(s)) && c.qty <= store_.qty(s)) {
      levels_[static_cast<std::size_t>(ix)].qty -= store_.qty(s) - c.qty;
      store_.qty(s) = c.qty;
      return sink.on_event(modified_event(symbol_, c.order_id, side, c.price, c.qty));
    }

    const OwnerId owner = store_.owner(s);
    ids_.erase_known(c.order_id, s);
    unlink(s);
    store_.release(s);
    sink.on_event(modified_event(symbol_, c.order_id, side, c.price, c.qty));
    if (side == Side::Buy)
      reenter<Side::Buy>(c.order_id, owner, ix, c.qty, c.ts, sink);
    else
      reenter<Side::Sell>(c.order_id, owner, ix, c.qty, c.ts, sink);
  }

  // ---- queries (not on the hot path) ----

  std::optional<Price> best_bid() const noexcept {
    return best_bid_ >= 0 ? std::optional<Price>(min_price_ + best_bid_) : std::nullopt;
  }
  std::optional<Price> best_ask() const noexcept {
    return best_ask_ < n_ ? std::optional<Price>(min_price_ + best_ask_) : std::nullopt;
  }
  std::uint64_t best_bid_qty() const noexcept { return best_bid_ >= 0 ? level(best_bid_).qty : 0; }
  std::uint64_t best_ask_qty() const noexcept { return best_ask_ < n_ ? level(best_ask_).qty : 0; }
  std::size_t order_count() const noexcept { return store_.live(); }
  bool contains(OrderId id) const noexcept { return ids_.find(id, key_of()) != kNil; }
  SymbolId symbol() const noexcept { return symbol_; }

  std::optional<OrderView> find_order(OrderId id) const noexcept {
    const std::uint32_t s = ids_.find(id, key_of());
    if (s == kNil) return std::nullopt;
    return OrderView{min_price_ + store_.level(s), store_.qty(s), store_.side(s), store_.owner(s)};
  }

  // Visits resting orders of one side in priority order (best price first, FIFO within a level).
  template <class F>
  void for_each_order(Side side, F&& f) const {
    if (side == Side::Buy) {
      for (std::int32_t i = best_bid_; i >= 0; i = bid_bits_.find_prev(i - 1)) visit_level(i, f);
    } else {
      for (std::int32_t i = best_ask_; i < n_; i = ask_bits_.find_next(i + 1)) visit_level(i, f);
    }
  }

  // Visits the price levels of one side, best first: f(Price, total quantity). Market data uses it; the matching
  // path never does.
  template <class F>
  void for_each_level(Side side, F&& f) const {
    if (side == Side::Buy) {
      for (std::int32_t i = best_bid_; i >= 0; i = bid_bits_.find_prev(i - 1)) f(min_price_ + i, level(i).qty);
    } else {
      for (std::int32_t i = best_ask_; i < n_; i = ask_bits_.find_next(i + 1)) f(min_price_ + i, level(i).qty);
    }
  }

  // Total resting quantity at a price on one side (0 if none or out of band).
  Qty level_qty(Side side, Price px) const noexcept {
    const Price rel = px - min_price_;
    if (rel < 0 || rel >= n_) return 0;
    const auto i = static_cast<std::int32_t>(rel);
    return (side == Side::Buy ? bid_bits_.test(i) : ask_bits_.test(i)) ? level(i).qty : 0;
  }

  // Visits the queue at one price, front first: f(OrderId, Qty, Side). Out-of-band prices visit nothing.
  template <class F>
  void for_each_order_at(Price px, F&& f) const {
    const Price rel = px - min_price_;
    if (rel < 0 || rel >= n_) return;
    for (std::uint32_t s = level(static_cast<std::int32_t>(rel)).head; s != kNil; s = store_.next(s))
      f(store_.id(s), store_.qty(s), store_.side(s));
  }

  // Full structural audit, O(num_levels + orders). Tests call it; the engine never does.
  bool check_invariants() const {
    if (best_bid_ != bid_bits_.find_prev(n_ - 1) || best_ask_ != ask_bits_.find_next(0)) return false;
    if (best_bid_ >= 0 && best_ask_ < n_ && best_bid_ >= best_ask_) return false;  // crossed
    std::size_t seen = 0;
    for (std::int32_t i = 0; i < n_; ++i) {
      const Level& lv = level(i);
      const bool b = bid_bits_.test(i), a = ask_bits_.test(i);
      if (b && a) return false;
      if (!b && !a) {
        if (lv.head != kNil || lv.tail != kNil || lv.qty != 0) return false;
        continue;
      }
      const Side side = b ? Side::Buy : Side::Sell;
      if (lv.head == kNil) return false;
      std::uint64_t sum = 0;
      std::uint32_t last = kNil;
      for (std::uint32_t s = lv.head; s != kNil; s = store_.next(s)) {
        if (store_.side(s) != side || store_.level(s) != static_cast<std::uint32_t>(i) || store_.qty(s) == 0) return false;
        if (last != kNil && store_.prev(s) != last) return false;
        if (ids_.find(store_.id(s), key_of()) != s) return false;
        sum += store_.qty(s);
        last = s;
        if (++seen > store_.live()) return false;  // cycle
      }
      if (last != lv.tail || sum != lv.qty) return false;
    }
    return seen == store_.live() && seen == ids_.size();
  }

 private:
  // Resolves an index slot's order id through the store, for fingerprint verification.
  EXSIM_ALWAYS_INLINE auto key_of() const noexcept {
    return [this](std::uint32_t s) noexcept { return store_.id(s); };
  }

  EXSIM_ALWAYS_INLINE Level& level(std::int32_t ix) noexcept { return levels_[static_cast<std::size_t>(ix)]; }
  EXSIM_ALWAYS_INLINE const Level& level(std::int32_t ix) const noexcept {
    return levels_[static_cast<std::size_t>(ix)];
  }

  template <Side S>
  EXSIM_ALWAYS_INLINE bool crosses(std::int32_t limit) const noexcept {
    if constexpr (S == Side::Buy)
      return best_ask_ <= limit;
    else
      return best_bid_ >= limit;
  }

  template <Side S, class Sink>
  EXSIM_ALWAYS_INLINE void add_side(const Command& c, Sink& sink) noexcept {
    constexpr bool kBuy = S == Side::Buy;
    if (EXSIM_UNLIKELY(c.order_id == 0)) return sink.on_event(rejected_event(c, Reason::InvalidOrderId));
    if (EXSIM_UNLIKELY(c.qty == 0)) return sink.on_event(rejected_event(c, Reason::InvalidQty));
    const bool market = c.ord_type == OrdType::Market;
    std::int32_t limit;
    if (market) {
      limit = kBuy ? n_ - 1 : 0;
    } else {
      const Price rel = c.price - min_price_;
      if (EXSIM_UNLIKELY(rel < 0 || rel >= n_)) return sink.on_event(rejected_event(c, Reason::InvalidPrice));
      limit = static_cast<std::int32_t>(rel);
    }
    if (EXSIM_UNLIKELY(ids_.find(c.order_id, key_of()) != kNil))
      return sink.on_event(rejected_event(c, Reason::DuplicateOrderId));
    if (EXSIM_UNLIKELY((c.flags & kFlagPostOnly) != 0) && crosses<S>(limit))
      return sink.on_event(rejected_event(c, Reason::WouldCross));

    const Price px = market ? 0 : c.price;
    sink.on_event(accepted_event(symbol_, c.order_id, S, px, c.qty));

    const Stp stp = effective_stp(c.flags, stp_);
    if (EXSIM_UNLIKELY(c.tif == Tif::Fok) && !fillable<S>(limit, c.qty, c.owner, stp)) {
      return sink.on_event(canceled_event(symbol_, c.order_id, S, px, c.qty, Reason::FokUnfilled));
    }
    bool stp_stop = false;
    const Qty left = sweep<S>(c.order_id, c.owner, limit, c.qty, stp, stp_stop, sink);
    if (left == 0) return;
    if (EXSIM_UNLIKELY(stp_stop))
      return sink.on_event(canceled_event(symbol_, c.order_id, S, px, left, Reason::SelfTrade));
    if (market || c.tif != Tif::Day)
      return sink.on_event(canceled_event(symbol_, c.order_id, S, px, left, Reason::IocExpired));
    rest<S>(c.order_id, c.owner, limit, left, c.ts, sink);
  }

  template <Side S, class Sink>
  EXSIM_ALWAYS_INLINE void reenter(OrderId id, OwnerId owner, std::int32_t ix, Qty qty, std::uint64_t ts,
                                   Sink& sink) noexcept {
    bool stp_stop = false;
    const Qty left = sweep<S>(id, owner, ix, qty, stp_, stp_stop, sink);
    if (left == 0) return;
    if (EXSIM_UNLIKELY(stp_stop))
      return sink.on_event(canceled_event(symbol_, id, S, min_price_ + ix, left, Reason::SelfTrade));
    rest<S>(id, owner, ix, left, ts, sink);
  }

  // Matches an incoming order of side S against the opposite side, up to `limit`. Returns the
  // unfilled quantity. This loop is the hot path. For each maker it reads {id, qty, next}, emits a
  // trade, then either decrements the maker in place or releases it.
  template <Side S, class Sink>
  EXSIM_ALWAYS_INLINE Qty sweep(OrderId taker, OwnerId taker_owner, std::int32_t limit, Qty qty, Stp stp,
                                bool& stp_stop, Sink& sink) noexcept {
    constexpr bool kBuy = S == Side::Buy;
    std::int32_t& best = kBuy ? best_ask_ : best_bid_;
    while (qty != 0 && crosses<S>(limit)) {
      const std::int32_t ix = best;
      Level& lv = level(ix);
      const Price px = min_price_ + ix;
      std::uint32_t s = lv.head;
      while (s != kNil) {
        if (EXSIM_UNLIKELY(stp != Stp::None) && store_.owner(s) == taker_owner) {
          if (stp == Stp::CancelIncoming) {
            stp_stop = true;
            break;
          }
          const Qty mq = store_.qty(s);
          if (stp == Stp::DecrementCancel && mq > qty) {
            // The resting order is larger: it is decremented by the incoming remainder, which is cancelled.
            store_.qty(s) = mq - qty;
            lv.qty -= qty;
            sink.on_event(stp_decrement_event(symbol_, store_.id(s), opposite(S), px, mq - qty));
            stp_stop = true;
            break;
          }
          // Cancel the resting order (CancelResting, or DecrementCancel with a smaller or equal resting order).
          const std::uint32_t nx = store_.next(s);
          lv.qty -= mq;
          sink.on_event(canceled_event(symbol_, store_.id(s), opposite(S), px, mq, Reason::SelfTrade));
          ids_.erase_known(store_.id(s), s);
          store_.release(s);
          s = nx;
          if (stp == Stp::DecrementCancel) {
            if (mq == qty) {  // equal: both are cancelled
              stp_stop = true;
              break;
            }
            qty -= mq;  // the incoming order is decremented by the cancelled resting order's size
            sink.on_event(stp_decrement_event(symbol_, taker, S, min_price_ + limit, qty));
          }
          continue;
        }
        const Qty avail = store_.qty(s);
        const Qty fill = avail < qty ? avail : qty;
        const Qty left = avail - fill;
        qty -= fill;
        lv.qty -= fill;
        sink.on_event(trade_event(symbol_, taker, store_.id(s), S, px, fill, left));
        if (left != 0) {  // maker partially filled; taker is done
          store_.qty(s) = left;
          break;
        }
        const std::uint32_t nx = store_.next(s);
        ids_.erase_known(store_.id(s), s);
        store_.release(s);
        s = nx;
        if (qty == 0) break;
      }
      lv.head = s;
      if (s == kNil) {
        lv.tail = kNil;
        if constexpr (kBuy) {
          ask_bits_.clear(ix);
          best = ask_bits_.find_next(ix + 1);
        } else {
          bid_bits_.clear(ix);
          best = bid_bits_.find_prev(ix - 1);
        }
      }
      if (stp_stop) break;
    }
    return qty;
  }

  // FOK pre-check: would the sweep resolve all of `qty` within `limit` without the order being cancelled?
  // Level aggregates answer this unless self-trade prevention is on. With STP on, the check simulates the
  // sweep order by order: own orders provide no liquidity (CancelResting), end it (CancelIncoming), or
  // decrement the order (DecrementCancel; decremented quantity counts as resolved, a cancel does not).
  template <Side S>
  bool fillable(std::int32_t limit, Qty qty, OwnerId owner, Stp stp) const noexcept {
    std::uint64_t need = qty;
    for (std::int32_t i = S == Side::Buy ? best_ask_ : best_bid_;
         S == Side::Buy ? (i <= limit) : (i >= limit && i >= 0);
         i = S == Side::Buy ? ask_bits_.find_next(i + 1) : bid_bits_.find_prev(i - 1)) {
      const Level& lv = level(i);
      if (stp == Stp::None) {
        if (lv.qty >= need) return true;
        need -= lv.qty;
        continue;
      }
      for (std::uint32_t s = lv.head; s != kNil; s = store_.next(s)) {
        const Qty q = store_.qty(s);
        if (store_.owner(s) == owner) {
          if (stp == Stp::CancelIncoming) return false;
          if (stp == Stp::DecrementCancel) {
            if (q >= need) return false;  // the incoming order would be cancelled
            need -= q;
          }
          continue;
        }
        if (q >= need) return true;
        need -= q;
      }
    }
    return false;
  }

  template <Side S, class Sink>
  EXSIM_ALWAYS_INLINE void rest(OrderId id, OwnerId owner, std::int32_t ix, Qty qty, std::uint64_t ts,
                                Sink& sink) noexcept {
    const std::uint32_t s = store_.alloc();
    if (EXSIM_UNLIKELY(s == kNil))
      return sink.on_event(canceled_event(symbol_, id, S, min_price_ + ix, qty, Reason::BookFull));
    store_.init(s, id, qty, static_cast<std::uint32_t>(ix), owner, S, ts);
    Level& lv = level(ix);
    store_.next(s) = kNil;
    store_.prev(s) = lv.tail;
    if (lv.tail == kNil) {
      lv.head = s;
      if constexpr (S == Side::Buy) {
        bid_bits_.set(ix);
        if (ix > best_bid_) best_bid_ = ix;
      } else {
        ask_bits_.set(ix);
        if (ix < best_ask_) best_ask_ = ix;
      }
    } else {
      store_.next(lv.tail) = s;
    }
    lv.tail = s;
    lv.qty += qty;
    ids_.insert(id, s);
  }

  // Removes slot s from its level's FIFO (O(1)); retires the level if it empties.
  EXSIM_ALWAYS_INLINE void unlink(std::uint32_t s) noexcept {
    const auto ix = static_cast<std::int32_t>(store_.level(s));
    Level& lv = level(ix);
    const std::uint32_t n = store_.next(s);
    const std::uint32_t p = store_.prev(s);
    if (s == lv.head)
      lv.head = n;
    else
      store_.next(p) = n;
    if (s == lv.tail)
      lv.tail = lv.head == kNil ? kNil : p;
    else
      store_.prev(n) = p;  // n may now be the head; a head's prev is never read
    lv.qty -= store_.qty(s);
    if (lv.head == kNil) {
      if (store_.side(s) == Side::Buy) {
        bid_bits_.clear(ix);
        if (ix == best_bid_) best_bid_ = bid_bits_.find_prev(ix - 1);
      } else {
        ask_bits_.clear(ix);
        if (ix == best_ask_) best_ask_ = ask_bits_.find_next(ix + 1);
      }
    }
  }

  template <class F>
  void visit_level(std::int32_t i, F& f) const {
    for (std::uint32_t s = level(i).head; s != kNil; s = store_.next(s))
      f(min_price_ + i, store_.id(s), store_.qty(s));
  }

  // Hot scalars first: they share one cache line.
  const SymbolId symbol_;
  const Price min_price_;
  const std::int32_t n_;
  const Stp stp_;
  std::int32_t best_bid_;
  std::int32_t best_ask_;

  PageArray<Level> levels_;
  PriceBitmap bid_bits_;
  PriceBitmap ask_bits_;
  OrderIndex<IndexHash> ids_;
  Store store_;
};

}  // namespace exsim
