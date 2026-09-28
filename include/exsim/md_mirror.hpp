// Reconstructs an exchange's order book inside our matching engine from level-2 data.
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "exsim/md.hpp"
#include "exsim/order_book.hpp"
#include "exsim/sinks.hpp"

namespace exsim {

// Exchanges publish price-level totals (L2), while our engine matches orders. The bridge: each live
// price level is represented by ONE synthetic resting order whose quantity is the level total.
// Order ids are derived from (price, side), so they are unique and need no lookup table.
//
// Completeness horizon. A diff stream only reports levels that CHANGE. A level that existed when we
// seeded but lies deeper than the snapshot's last level, and never changes, is invisible to us, forever.
// So the mirror is provably complete only between the touch and the seed snapshot's deepest level on each
// side (`bid_horizon()` / `ask_horizon()`); beyond that it is a partial view. As the market moves, the
// complete region shrinks on one side. `needs_reseed()` says when half of it is gone; callers then reseed
// from a fresh snapshot (exact, since snapshots can be aligned to diff boundaries).
//
// A single 100 ms diff can move the whole book (the mid ticks up: old asks vanish while new bids
// appear above the old best ask). If additions were applied before removals, that batch would
// transiently cross the book and the engine would generate phantom trades. Removals and reductions
// are therefore applied across BOTH sides before any additions. For any diff whose end state is
// uncrossed (all real ones), this makes crossing impossible. `phantom_trades()` counts violations and
// must stay 0.
class BookMirror {
 public:
  using Book = OrderBook<HybridStore>;

  BookMirror(std::int64_t center_px, std::uint32_t num_levels = 1u << 20, std::uint32_t max_orders = 1u << 18)
      : min_px_(center_px - static_cast<std::int64_t>(num_levels / 2)),
        n_(num_levels),
        max_orders_(max_orders),
        bid_qty_(num_levels, 0),
        ask_qty_(num_levels, 0) {
    reset_book();
  }

  // Drops all state and reseeds from a snapshot.
  void seed(const md::Record& snap) {
    std::fill(bid_qty_.begin(), bid_qty_.end(), 0);
    std::fill(ask_qty_.begin(), ask_qty_.end(), 0);
    reset_book();
    for (std::uint32_t i = 0; i < snap.n_bids; ++i) set_level(Side::Buy, snap.bid(i), /*allow_add=*/true);
    for (std::uint32_t i = 0; i < snap.n_asks; ++i) set_level(Side::Sell, snap.ask(i), true);
    bid_horizon_ = snap.n_bids ? snap.bid(snap.n_bids - 1).px : min_px_;
    ask_horizon_ = snap.n_asks ? snap.ask(snap.n_asks - 1).px : min_px_ + static_cast<std::int64_t>(n_);
    bid_span0_ = snap.n_bids ? snap.bid(0).px - bid_horizon_ : 0;
    ask_span0_ = snap.n_asks ? ask_horizon_ - snap.ask(0).px : 0;
  }

  std::int64_t bid_horizon() const { return bid_horizon_; }  // deepest bid the mirror is complete down to
  std::int64_t ask_horizon() const { return ask_horizon_; }  // deepest ask the mirror is complete up to

  // True once the market has consumed half of the seeded depth on either side.
  bool needs_reseed() const {
    const auto bb = book_->best_bid(), ba = book_->best_ask();
    if (!bb || !ba) return true;
    return (ask_horizon_ - *ba) * 2 < ask_span0_ || (*bb - bid_horizon_) * 2 < bid_span0_;
  }

  // Applies one diff. `drop_every` > 0 silently discards every Nth level update (fault injection for
  // proving the validator can fail).
  void apply(const md::Record& diff, std::uint64_t drop_every = 0) {
    for (int pass = 0; pass < 2; ++pass) {
      for (std::uint32_t i = 0; i < diff.n_bids; ++i) touch(Side::Buy, diff.bid(i), pass, drop_every);
      for (std::uint32_t i = 0; i < diff.n_asks; ++i) touch(Side::Sell, diff.ask(i), pass, drop_every);
    }
  }

  const Book& book() const { return *book_; }
  std::uint64_t level_updates() const { return updates_; }
  std::uint64_t out_of_band() const { return out_of_band_; }
  std::uint64_t phantom_trades() const { return sink_.count(EventType::Trade); }
  std::uint64_t rejects() const { return sink_.count(EventType::Rejected); }
  std::uint64_t book_full() const { return book_full_; }

  // Quantity resting at a price (0 if none).
  std::uint64_t qty_at(Side side, std::int64_t px) const {
    const std::int64_t rel = px - min_px_;
    if (rel < 0 || rel >= static_cast<std::int64_t>(n_)) return 0;
    return (side == Side::Buy ? bid_qty_ : ask_qty_)[static_cast<std::size_t>(rel)];
  }

  // The top `n` price levels of one side, best first, as (price, total qty). With `within_horizon`,
  // only levels inside the region the mirror is provably complete for.
  std::vector<md::LevelUpdate> top(Side side, std::size_t n, bool within_horizon = false) const {
    std::vector<md::LevelUpdate> out;
    book_->for_each_order(side, [&](Price p, OrderId, Qty q) {
      if (within_horizon && (side == Side::Buy ? p < bid_horizon_ : p > ask_horizon_)) return;
      if (!out.empty() && out.back().px == p) {
        out.back().qty += q;
      } else if (out.size() < n) {
        out.push_back({p, q});
      }
    });
    return out;
  }

 private:
  void reset_book() {
    BookConfig cfg;
    cfg.min_price = min_px_;
    cfg.num_levels = n_;
    cfg.max_orders = max_orders_;
    cfg.stp = Stp::None;
    book_ = std::make_unique<Book>(0, cfg);
    sink_ = CountingSink{};
  }

  static OrderId id_for(std::int64_t rel, Side side) { return (static_cast<OrderId>(rel) << 1 | (side == Side::Sell ? 1 : 0)) + 1; }

  std::uint64_t& slot(Side side, std::int64_t rel) { return (side == Side::Buy ? bid_qty_ : ask_qty_)[static_cast<std::size_t>(rel)]; }

  void touch(Side side, md::LevelUpdate u, int pass, std::uint64_t drop_every) {
    if (drop_every != 0 && (++drop_counter_ % drop_every) == 0) return;
    const std::int64_t rel = u.px - min_px_;
    if (rel < 0 || rel >= static_cast<std::int64_t>(n_)) {
      if (pass == 0) ++out_of_band_;
      return;
    }
    const std::uint64_t cur = slot(side, rel);
    const bool reduces = u.qty < cur;  // includes deletions
    if (pass == 0 && reduces) set_level(side, u, false);
    if (pass == 1 && !reduces) set_level(side, u, true);
  }

  void set_level(Side side, md::LevelUpdate u, bool allow_add) {
    const std::int64_t rel = u.px - min_px_;
    if (rel < 0 || rel >= static_cast<std::int64_t>(n_)) {
      ++out_of_band_;
      return;
    }
    ++updates_;
    std::uint64_t& cur = slot(side, rel);
    const OrderId id = id_for(rel, side);
    Command c{};
    c.order_id = id;
    c.side = side;
    c.price = u.px;
    c.qty = static_cast<Qty>(u.qty);
    if (u.qty == 0) {
      if (cur != 0) {
        c.type = MsgType::Cancel;
        book_->cancel(c, sink_);
      }
    } else if (cur == 0) {
      if (!allow_add) return;
      c.type = MsgType::NewOrder;
      book_->add(c, sink_);
      if (!book_->contains(id)) ++book_full_;
    } else {
      c.type = MsgType::Modify;  // shrink keeps priority; growth re-queues (irrelevant for one order per level)
      book_->modify(c, sink_);
    }
    cur = u.qty;
  }

  std::int64_t min_px_;
  std::uint32_t n_, max_orders_;
  std::vector<std::uint64_t> bid_qty_, ask_qty_;
  std::unique_ptr<Book> book_;
  CountingSink sink_;
  std::uint64_t updates_ = 0, out_of_band_ = 0, book_full_ = 0, drop_counter_ = 0;
  std::int64_t bid_horizon_ = 0, ask_horizon_ = 0, bid_span0_ = 0, ask_span0_ = 0;
};

}  // namespace exsim
