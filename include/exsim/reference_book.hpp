// Deliberately naive order book: std::map of price -> std::list FIFO, plus std::unordered_map index.
//
// It has two jobs:
//   1. Oracle. Differential tests feed identical random streams to this book and to OrderBook<Store>
//      and require byte-identical event streams. The code here is the most obvious correct
//      transcription of the rules, written without regard to speed.
//   2. Baseline. Design point 0 in the benchmark suite: what the textbook implementation costs.
//
// Semantics, including validation order, must match order_book.hpp exactly.
#pragma once

#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

#include "exsim/common.hpp"

namespace exsim {

class ReferenceBook {
 public:
  ReferenceBook(SymbolId symbol, const BookConfig& cfg)
      : symbol_(symbol),
        min_price_(cfg.min_price),
        max_price_(cfg.min_price + static_cast<Price>(cfg.num_levels) - 1),
        capacity_(cfg.max_orders),
        stp_(cfg.stp) {}

  template <class Sink>
  void add(const Command& c, Sink& sink) {
    if (c.order_id == 0) return sink.on_event(rejected_event(c, Reason::InvalidOrderId));
    if (c.qty == 0) return sink.on_event(rejected_event(c, Reason::InvalidQty));
    const bool market = c.ord_type == OrdType::Market;
    Price limit;
    if (market) {
      limit = c.side == Side::Buy ? max_price_ : min_price_;
    } else {
      if (c.price < min_price_ || c.price > max_price_) return sink.on_event(rejected_event(c, Reason::InvalidPrice));
      limit = c.price;
    }
    if (index_.count(c.order_id) != 0) return sink.on_event(rejected_event(c, Reason::DuplicateOrderId));
    if ((c.flags & kFlagPostOnly) != 0 && crosses(c.side, limit))
      return sink.on_event(rejected_event(c, Reason::WouldCross));

    const Price px = market ? 0 : c.price;
    sink.on_event(accepted_event(symbol_, c.order_id, c.side, px, c.qty));
    const Stp stp = effective_stp(c.flags, stp_);
    if (c.tif == Tif::Fok && !fillable(c.side, limit, c.qty, c.owner, stp))
      return sink.on_event(canceled_event(symbol_, c.order_id, c.side, px, c.qty, Reason::FokUnfilled));

    bool stp_stop = false;
    const Qty left = match(c.order_id, c.owner, c.side, limit, c.qty, stp, stp_stop, sink);
    if (left == 0) return;
    if (stp_stop) return sink.on_event(canceled_event(symbol_, c.order_id, c.side, px, left, Reason::SelfTrade));
    if (market || c.tif != Tif::Day)
      return sink.on_event(canceled_event(symbol_, c.order_id, c.side, px, left, Reason::IocExpired));
    rest(c.order_id, c.owner, c.side, limit, left, sink);
  }

  template <class Sink>
  void cancel(const Command& c, Sink& sink) {
    auto it = index_.find(c.order_id);
    if (it == index_.end()) return sink.on_event(rejected_event(c, Reason::UnknownOrder));
    const Loc loc = it->second;
    const Qty leaves = loc.it->qty;
    remove(loc);
    index_.erase(it);
    sink.on_event(canceled_event(symbol_, c.order_id, loc.side, loc.price, leaves, Reason::UserCanceled));
  }

  template <class Sink>
  void modify(const Command& c, Sink& sink) {
    if (c.qty == 0) return sink.on_event(rejected_event(c, Reason::InvalidQty));
    auto it = index_.find(c.order_id);
    if (it == index_.end()) return sink.on_event(rejected_event(c, Reason::UnknownOrder));
    if (c.price < min_price_ || c.price > max_price_) return sink.on_event(rejected_event(c, Reason::InvalidPrice));
    const Loc loc = it->second;
    if (c.price == loc.price && c.qty <= loc.it->qty) {
      loc.it->qty = c.qty;
      return sink.on_event(modified_event(symbol_, c.order_id, loc.side, c.price, c.qty));
    }
    const OwnerId owner = loc.it->owner;
    remove(loc);
    index_.erase(it);
    sink.on_event(modified_event(symbol_, c.order_id, loc.side, c.price, c.qty));
    bool stp_stop = false;
    const Qty left = match(c.order_id, owner, loc.side, c.price, c.qty, stp_, stp_stop, sink);
    if (left == 0) return;
    if (stp_stop) return sink.on_event(canceled_event(symbol_, c.order_id, loc.side, c.price, left, Reason::SelfTrade));
    rest(c.order_id, owner, loc.side, c.price, left, sink);
  }

  std::optional<Price> best_bid() const { return bids_.empty() ? std::nullopt : std::optional(bids_.begin()->first); }
  std::optional<Price> best_ask() const { return asks_.empty() ? std::nullopt : std::optional(asks_.begin()->first); }
  std::uint64_t best_bid_qty() const { return bids_.empty() ? 0 : level_qty(bids_.begin()->second); }
  std::uint64_t best_ask_qty() const { return asks_.empty() ? 0 : level_qty(asks_.begin()->second); }
  std::size_t order_count() const { return index_.size(); }
  bool contains(OrderId id) const { return index_.count(id) != 0; }
  SymbolId symbol() const { return symbol_; }

  std::optional<OrderView> find_order(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    return OrderView{it->second.price, it->second.it->qty, it->second.side, it->second.it->owner};
  }

  template <class F>
  void for_each_order(Side side, F&& f) const {
    auto visit = [&](const auto& book) {
      for (const auto& [px, q] : book)
        for (const Order& o : q) f(px, o.id, o.qty);
    };
    if (side == Side::Buy)
      visit(bids_);
    else
      visit(asks_);
  }

  bool check_invariants() const {
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) return false;
    std::size_t n = 0;
    for (const auto& [px, q] : bids_) n += q.size();
    for (const auto& [px, q] : asks_) n += q.size();
    return n == index_.size();
  }

 private:
  struct Order {
    OrderId id;
    Qty qty;
    OwnerId owner;
  };
  using Queue = std::list<Order>;
  struct Loc {
    Side side;
    Price price;
    Queue::iterator it;
  };

  static std::uint64_t level_qty(const Queue& q) {
    std::uint64_t s = 0;
    for (const Order& o : q) s += o.qty;
    return s;
  }

  bool crosses(Side side, Price limit) const {
    return side == Side::Buy ? !asks_.empty() && asks_.begin()->first <= limit
                             : !bids_.empty() && bids_.begin()->first >= limit;
  }

  bool fillable(Side side, Price limit, Qty qty, OwnerId owner, Stp stp) const {
    std::uint64_t need = qty;
    auto scan = [&](const auto& book, auto in_range) {
      for (const auto& [px, q] : book) {
        if (!in_range(px)) return false;
        for (const Order& o : q) {
          if (stp != Stp::None && o.owner == owner) {
            if (stp == Stp::CancelIncoming) return false;
            if (stp == Stp::DecrementCancel) {
              if (o.qty >= need) return false;
              need -= o.qty;
            }
            continue;
          }
          if (o.qty >= need) return true;
          need -= o.qty;
        }
      }
      return false;
    };
    if (side == Side::Buy) return scan(asks_, [&](Price p) { return p <= limit; });
    return scan(bids_, [&](Price p) { return p >= limit; });
  }

  template <class Sink>
  Qty match(OrderId taker, OwnerId owner, Side side, Price limit, Qty qty, Stp stp, bool& stp_stop, Sink& sink) {
    if (side == Side::Buy)
      return match_side(asks_, taker, owner, side, limit, [&](Price p) { return p <= limit; }, qty, stp, stp_stop, sink);
    return match_side(bids_, taker, owner, side, limit, [&](Price p) { return p >= limit; }, qty, stp, stp_stop, sink);
  }

  template <class Book, class InRange, class Sink>
  Qty match_side(Book& book, OrderId taker, OwnerId owner, Side side, Price limit, InRange in_range, Qty qty, Stp stp,
                 bool& stp_stop, Sink& sink) {
    while (qty > 0 && !book.empty() && in_range(book.begin()->first)) {
      auto lvl = book.begin();
      Queue& q = lvl->second;
      while (qty > 0 && !q.empty()) {
        Order& o = q.front();
        if (stp != Stp::None && o.owner == owner) {
          if (stp == Stp::CancelIncoming) {
            stp_stop = true;
            break;
          }
          if (stp == Stp::DecrementCancel && o.qty > qty) {
            o.qty -= qty;
            sink.on_event(stp_decrement_event(symbol_, o.id, opposite(side), lvl->first, o.qty));
            stp_stop = true;
            break;
          }
          const Qty mq = o.qty;
          sink.on_event(canceled_event(symbol_, o.id, opposite(side), lvl->first, mq, Reason::SelfTrade));
          index_.erase(o.id);
          q.pop_front();
          if (stp == Stp::DecrementCancel) {
            if (mq == qty) {
              stp_stop = true;
              break;
            }
            qty -= mq;
            sink.on_event(stp_decrement_event(symbol_, taker, side, limit, qty));
          }
          continue;
        }
        const Qty fill = std::min(o.qty, qty);
        o.qty -= fill;
        qty -= fill;
        sink.on_event(trade_event(symbol_, taker, o.id, side, lvl->first, fill, o.qty));
        if (o.qty == 0) {
          index_.erase(o.id);
          q.pop_front();
        }
      }
      if (q.empty()) book.erase(lvl);
      if (stp_stop) break;
    }
    return qty;
  }

  template <class Sink>
  void rest(OrderId id, OwnerId owner, Side side, Price px, Qty qty, Sink& sink) {
    if (index_.size() >= capacity_) return sink.on_event(canceled_event(symbol_, id, side, px, qty, Reason::BookFull));
    Queue& q = side == Side::Buy ? bids_[px] : asks_[px];
    q.push_back(Order{id, qty, owner});
    index_.emplace(id, Loc{side, px, std::prev(q.end())});
  }

  void remove(const Loc& loc) {
    auto erase_from = [&](auto& book) {
      auto lvl = book.find(loc.price);
      lvl->second.erase(loc.it);
      if (lvl->second.empty()) book.erase(lvl);
    };
    if (loc.side == Side::Buy)
      erase_from(bids_);
    else
      erase_from(asks_);
  }

  SymbolId symbol_;
  Price min_price_;
  Price max_price_;
  std::size_t capacity_;
  Stp stp_;
  std::map<Price, Queue, std::greater<>> bids_;
  std::map<Price, Queue> asks_;
  std::unordered_map<OrderId, Loc> index_;
};

}  // namespace exsim
