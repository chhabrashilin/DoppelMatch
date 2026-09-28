// Order-by-order (L3) exchange data: the .exl3 record stream and a "truth book" rebuilt from it.
//
// The truth book contains no matching logic. It applies the exchange's own messages literally: an order
// joins the back of its price level when the exchange says it is `open`, shrinks when the exchange reports
// a `match` against it, and leaves when the exchange says it is `done`. Queue order is therefore the
// exchange's queue order, which makes it ground truth for two things this project could previously only
// estimate: what the matching engine should do next (tools/exsim_l3replay.cpp) and where a resting order
// really sits in the queue (the queue-model study).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "exsim/common.hpp"

namespace exsim::l3 {

enum Kind : std::uint8_t { SnapOrder = 1, SnapEnd = 2, Received = 3, Open = 4, Done = 5, Match = 6, Change = 7 };
enum Flag : std::uint8_t { FundsOnly = 1, Canceled = 2, Stp = 4, FundsChange = 8, HasPrice = 16 };

// One exchange message, 72 bytes, written by scripts/l3conv.py.
struct Rec {
  std::uint64_t seq;    // exchange sequence number (per product, contiguous)
  std::uint64_t ts;     // exchange timestamp, ns since the Unix epoch
  std::uint64_t id;     // order id (Match: taker)
  std::uint64_t id2;    // Match: maker
  std::int64_t px;      // price in ticks (Change: new price)
  std::int64_t px2;     // Change: old price
  std::uint64_t q;      // size in lots (Received: size; Open/Done: remaining; Match: fill; Change: new size)
  std::uint64_t q2;     // Change: old size
  std::uint8_t kind;
  std::uint8_t side;    // 0 buy, 1 sell (Match: the maker's side)
  std::uint8_t otype;   // Received: 0 limit, 1 market
  std::uint8_t flags;
  std::uint32_t pad;
};
static_assert(sizeof(Rec) == 72);

// Sequential reader with a large buffer: a day of Coinbase BTC-USD is tens of millions of records, so
// the file is streamed, never loaded whole.
class Reader {
 public:
  explicit Reader(const std::string& path) : f_(std::fopen(path.c_str(), "rb")), buf_(kBatch) {
    if (f_ == nullptr) throw std::runtime_error("cannot open " + path);
    char hdr[16];
    if (std::fread(hdr, 1, 16, f_) != 16 || std::memcmp(hdr, "EXL3", 4) != 0) throw std::runtime_error("not an EXL3 file");
    std::memcpy(&price_dec_, hdr + 8, 4);
    std::memcpy(&qty_dec_, hdr + 12, 4);
  }
  ~Reader() { if (f_) std::fclose(f_); }
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // Returns the next record, or nullptr at end of file.
  const Rec* next() {
    if (pos_ == len_) {
      len_ = std::fread(buf_.data(), sizeof(Rec), kBatch, f_);
      pos_ = 0;
      if (len_ == 0) return nullptr;
    }
    return &buf_[pos_++];
  }
  std::uint32_t price_decimals() const { return price_dec_; }
  std::uint32_t qty_decimals() const { return qty_dec_; }

 private:
  static constexpr std::size_t kBatch = 1 << 16;
  std::FILE* f_;
  std::vector<Rec> buf_;
  std::size_t pos_ = 0, len_ = 0;
  std::uint32_t price_dec_ = 0, qty_dec_ = 0;
};

// The exchange's book, rebuilt from its own messages. Not performance critical: it is the reference.
class TruthBook {
 public:
  struct Order {
    OrderId id;
    Qty qty;
    std::uint64_t since;  // exchange time the order joined its current queue position
  };
  using Queue = std::list<Order>;

  void clear() {
    bids_.clear();
    asks_.clear();
    where_.clear();
    bid_qty_.clear();
    ask_qty_.clear();
  }

  // Joins the back of the queue at px (snapshot orders arrive in queue order, so this is exact too).
  void add(OrderId id, Side side, Price px, Qty qty, std::uint64_t ts) {
    Queue& q = level(side, px);
    q.push_back(Order{id, qty, ts});
    where_[id] = Loc{side, px, std::prev(q.end())};
    totals(side)[px] += qty;
  }

  // A match against a resting order. Returns false if the maker is unknown (a data inconsistency).
  bool fill(OrderId maker, Qty qty) {
    auto it = where_.find(maker);
    if (it == where_.end() || it->second.it->qty < qty) return false;
    it->second.it->qty -= qty;
    totals(it->second.side)[it->second.px] -= qty;
    return true;
  }

  // Total resting quantity at a price (0 if none).
  Qty level_qty(Side side, Price px) const {
    const auto& t = side == Side::Buy ? bid_qty_ : ask_qty_;
    const auto it = t.find(px);
    return it == t.end() ? 0 : it->second;
  }
  // Visits up to n levels of one side, best first: f(price, total quantity).
  template <class F>
  void for_each_level(Side side, std::size_t n, F&& f) const {
    if (side == Side::Buy) {
      for (auto it = bids_.begin(); it != bids_.end() && n > 0; ++it, --n) f(it->first, bid_qty_.at(it->first));
    } else {
      for (auto it = asks_.begin(); it != asks_.end() && n > 0; ++it, --n) f(it->first, ask_qty_.at(it->first));
    }
  }

  // The exchange says the order is finished. Absent orders (never rested) are fine.
  void remove(OrderId id) {
    auto it = where_.find(id);
    if (it == where_.end()) return;
    erase_at(it->second);
    where_.erase(it);
  }

  // Coinbase modify semantics (its FIX and REST docs): a size decrease at the same price keeps queue
  // priority; a size increase or any price change moves the order to the back of the queue at its new
  // price. Self-trade-prevention changes are always decreases.
  void change(OrderId id, Price new_px, Qty new_qty, std::uint64_t ts) {
    auto it = where_.find(id);
    if (it == where_.end()) return;
    Loc& loc = it->second;
    if (new_px == loc.px && new_qty <= loc.it->qty) {
      totals(loc.side)[loc.px] -= loc.it->qty - new_qty;
      loc.it->qty = new_qty;
      return;
    }
    const Side side = loc.side;
    erase_at(loc);
    where_.erase(it);
    add(id, side, new_px, new_qty, ts);
  }

  bool contains(OrderId id) const { return where_.count(id) != 0; }
  const Order* find(OrderId id, Side* side = nullptr, Price* px = nullptr) const {
    auto it = where_.find(id);
    if (it == where_.end()) return nullptr;
    if (side) *side = it->second.side;
    if (px) *px = it->second.px;
    return &*it->second.it;
  }
  std::size_t size() const { return where_.size(); }

  // Visits one side best price first, FIFO within a level: f(price, order).
  template <class F>
  void for_each(Side side, F&& f) const {
    if (side == Side::Buy) {
      for (const auto& [px, q] : bids_) for (const Order& o : q) f(px, o);
    } else {
      for (const auto& [px, q] : asks_) for (const Order& o : q) f(px, o);
    }
  }

  // Queue at one price, front first (nullptr if empty).
  const Queue* queue(Side side, Price px) const {
    if (side == Side::Buy) {
      auto it = bids_.find(px);
      return it == bids_.end() ? nullptr : &it->second;
    }
    auto it = asks_.find(px);
    return it == asks_.end() ? nullptr : &it->second;
  }

  std::optional<Price> best(Side side) const {
    if (side == Side::Buy) return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
    return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
  }

 private:
  struct Loc {
    Side side;
    Price px;
    Queue::iterator it;
  };
  Queue& level(Side side, Price px) { return side == Side::Buy ? bids_[px] : asks_[px]; }
  std::unordered_map<Price, Qty>& totals(Side side) { return side == Side::Buy ? bid_qty_ : ask_qty_; }
  void erase_at(const Loc& loc) {
    auto& t = totals(loc.side);
    const auto tq = t.find(loc.px);
    tq->second -= loc.it->qty;
    if (loc.side == Side::Buy) {
      auto l = bids_.find(loc.px);
      l->second.erase(loc.it);
      if (l->second.empty()) bids_.erase(l), t.erase(tq);
    } else {
      auto l = asks_.find(loc.px);
      l->second.erase(loc.it);
      if (l->second.empty()) asks_.erase(l), t.erase(tq);
    }
  }

  std::map<Price, Queue, std::greater<>> bids_;
  std::map<Price, Queue> asks_;
  std::unordered_map<OrderId, Loc> where_;
  std::unordered_map<Price, Qty> bid_qty_, ask_qty_;  // level totals, kept in step with the queues
};

}  // namespace exsim::l3
