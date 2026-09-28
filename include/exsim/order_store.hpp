// Resting-order storage: preallocated slabs with an embedded free list, in three memory layouts.
//
// The order book reaches order fields only through accessors (id(s), qty(s), next(s), ...), so the
// physical layout is a compile-time policy. The three layouts below are drop-in interchangeable and
// are A/B benchmarked against each other (see BENCHMARKS.md):
//
//   AosStore     one 56-byte record per order (the conventional `struct Order`).
//   SoaStore     one array per field: pure structure-of-arrays.
//   HybridStore  hot/cold split: a 40-byte record with every field the engine touches after
//                insertion, plus a cold audit array (timestamp, original qty) written once and never
//                read on the hot path.
//
// A finer split (a 16 B {id, qty, next} "matching" record plus a 16 B {prev, level, owner, side}
// "link" record) was measured and rejected. Cancel, modify and insert need both halves, so it
// raised simulated D1 misses by 20% and LL misses by 59% relative to AoS (BENCHMARKS.md).
//
// Slots are 32-bit indices, not pointers. A free slot's `next` field is the free-list link, so the
// allocator needs no memory of its own. The free list is LIFO, so a newly rested order reuses the
// most recently freed (and therefore cache-warm) slot.
#pragma once

#include <cstdint>

#include "exsim/common.hpp"
#include "exsim/memory.hpp"

namespace exsim {

template <class Derived>
class FreeListStore {
 public:
  EXSIM_ALWAYS_INLINE std::uint32_t alloc() noexcept {
    const std::uint32_t s = free_;
    if (EXSIM_LIKELY(s != kNil)) {
      free_ = self().next(s);
      ++live_;
    }
    return s;
  }
  EXSIM_ALWAYS_INLINE void release(std::uint32_t s) noexcept {
    self().next(s) = free_;
    free_ = s;
    --live_;
  }
  std::uint32_t capacity() const noexcept { return cap_; }
  std::uint32_t live() const noexcept { return live_; }

 protected:
  void build_free_list(std::uint32_t cap) noexcept {
    cap_ = cap;
    for (std::uint32_t i = 0; i < cap; ++i) self().next(i) = i + 1 < cap ? i + 1 : kNil;
    free_ = cap != 0 ? 0 : kNil;
  }

 private:
  Derived& self() noexcept { return static_cast<Derived&>(*this); }
  std::uint32_t free_ = kNil;
  std::uint32_t cap_ = 0;
  std::uint32_t live_ = 0;
};

#define EXSIM_STORE_FIELD(name, type, expr)                                             \
  EXSIM_ALWAYS_INLINE type& name(std::uint32_t s) noexcept { return expr; }             \
  EXSIM_ALWAYS_INLINE const type& name(std::uint32_t s) const noexcept { return expr; }

class AosStore : public FreeListStore<AosStore> {
 public:
  static constexpr const char* kName = "aos";
  struct Order {
    OrderId id;
    std::uint64_t ts;
    Qty qty;
    Qty orig_qty;
    std::uint32_t next;
    std::uint32_t prev;
    std::uint32_t level;
    OwnerId owner;
    Side side;
    std::uint8_t flags;
  };
  static_assert(sizeof(Order) == 56);

  explicit AosStore(std::uint32_t cap) : o_(cap) { build_free_list(cap); }

  EXSIM_STORE_FIELD(id, OrderId, o_[s].id)
  EXSIM_STORE_FIELD(qty, Qty, o_[s].qty)
  EXSIM_STORE_FIELD(next, std::uint32_t, o_[s].next)
  EXSIM_STORE_FIELD(prev, std::uint32_t, o_[s].prev)
  EXSIM_STORE_FIELD(level, std::uint32_t, o_[s].level)
  EXSIM_STORE_FIELD(owner, OwnerId, o_[s].owner)
  EXSIM_STORE_FIELD(side, Side, o_[s].side)

  EXSIM_ALWAYS_INLINE void init(std::uint32_t s, OrderId id, Qty qty, std::uint32_t level, OwnerId owner, Side side,
                                std::uint64_t ts) noexcept {
    Order& o = o_[s];
    o.id = id, o.ts = ts, o.qty = qty, o.orig_qty = qty, o.level = level, o.owner = owner, o.side = side, o.flags = 0;
  }

 private:
  PageArray<Order> o_;
};

class SoaStore : public FreeListStore<SoaStore> {
 public:
  static constexpr const char* kName = "soa";

  explicit SoaStore(std::uint32_t cap)
      : id_(cap), ts_(cap), qty_(cap), orig_qty_(cap), next_(cap), prev_(cap), level_(cap), owner_(cap), side_(cap) {
    build_free_list(cap);
  }

  EXSIM_STORE_FIELD(id, OrderId, id_[s])
  EXSIM_STORE_FIELD(qty, Qty, qty_[s])
  EXSIM_STORE_FIELD(next, std::uint32_t, next_[s])
  EXSIM_STORE_FIELD(prev, std::uint32_t, prev_[s])
  EXSIM_STORE_FIELD(level, std::uint32_t, level_[s])
  EXSIM_STORE_FIELD(owner, OwnerId, owner_[s])
  EXSIM_STORE_FIELD(side, Side, side_[s])

  EXSIM_ALWAYS_INLINE void init(std::uint32_t s, OrderId id, Qty qty, std::uint32_t level, OwnerId owner, Side side,
                                std::uint64_t ts) noexcept {
    id_[s] = id, ts_[s] = ts, qty_[s] = qty, orig_qty_[s] = qty, level_[s] = level, owner_[s] = owner, side_[s] = side;
  }

 private:
  PageArray<OrderId> id_;
  PageArray<std::uint64_t> ts_;
  PageArray<Qty> qty_;
  PageArray<Qty> orig_qty_;
  PageArray<std::uint32_t> next_;
  PageArray<std::uint32_t> prev_;
  PageArray<std::uint32_t> level_;
  PageArray<OwnerId> owner_;
  PageArray<Side> side_;
};

class HybridStore : public FreeListStore<HybridStore> {
 public:
  static constexpr const char* kName = "hybrid";

  // Everything the engine reads or writes after insertion, packed to 40 bytes (64-bit id and qty, four
  // 32-bit links/ids, side and flags). It was 32 bytes while quantities were 32-bit.
  struct Record {
    OrderId id;
    Qty qty;
    std::uint32_t next;
    std::uint32_t prev;
    std::uint32_t level;
    OwnerId owner;
    Side side;
    std::uint8_t flags;
    std::uint16_t pad;
  };
  // Written once on insert and never read by the engine (audit and reporting only).
  struct Audit {
    std::uint64_t ts;
    Qty orig_qty;
  };
  static_assert(sizeof(Record) == 40 && sizeof(Audit) == 16);

  explicit HybridStore(std::uint32_t cap) : rec_(cap), audit_(cap) { build_free_list(cap); }

  EXSIM_STORE_FIELD(id, OrderId, rec_[s].id)
  EXSIM_STORE_FIELD(qty, Qty, rec_[s].qty)
  EXSIM_STORE_FIELD(next, std::uint32_t, rec_[s].next)
  EXSIM_STORE_FIELD(prev, std::uint32_t, rec_[s].prev)
  EXSIM_STORE_FIELD(level, std::uint32_t, rec_[s].level)
  EXSIM_STORE_FIELD(owner, OwnerId, rec_[s].owner)
  EXSIM_STORE_FIELD(side, Side, rec_[s].side)

  EXSIM_ALWAYS_INLINE void init(std::uint32_t s, OrderId id, Qty qty, std::uint32_t level, OwnerId owner, Side side,
                                std::uint64_t ts) noexcept {
    Record& r = rec_[s];
    r.id = id, r.qty = qty, r.level = level, r.owner = owner, r.side = side, r.flags = 0;
    Audit& a = audit_[s];
    a.ts = ts, a.orig_qty = qty;
  }

 private:
  PageArray<Record> rec_;
  PageArray<Audit> audit_;
};

#undef EXSIM_STORE_FIELD

}  // namespace exsim
