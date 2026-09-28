// Order id -> store slot index: open addressing, linear probing, backward-shift deletion.
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <utility>

#include "exsim/common.hpp"
#include "exsim/memory.hpp"

namespace exsim {

// Hash policies. Each maps an order id to a 32-bit fingerprint; the low bits of the fingerprint pick
// the home slot. This choice turned out to be one of the most consequential in the engine
// (BENCHMARKS.md, design points 1 -> 2).
//
// ScatterHash (Murmur3 fmix64) spreads keys uniformly: the textbook choice. But the table is sized
// for the book's *capacity* (2^18 orders -> 2^19 slots), while only a few thousand orders are live.
// With uniform scattering, every lookup lands on a cold cache line and usually a cold page.
//
// LocalityHash keeps the low bits of the id (folded with the bits above the table size). Order ids
// are issued roughly sequentially, so the orders live at any moment occupy a compact, cache-resident
// window of the table, whatever the capacity. The trade-off is that a participant choosing ids
// adversarially (strided by the table size) could build long probe chains. Venues avoid this by
// assigning order ids themselves. A deployment that accepts arbitrary client ids should use
// ScatterHash, or salt the fold.
struct ScatterHash {
  static constexpr const char* kName = "scatter";
  EXSIM_ALWAYS_INLINE static constexpr std::uint32_t fingerprint(std::uint64_t k, int) noexcept {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ull;
    k ^= k >> 33;
    return static_cast<std::uint32_t>(k);
  }
};

struct LocalityHash {
  static constexpr const char* kName = "locality";
  EXSIM_ALWAYS_INLINE static constexpr std::uint32_t fingerprint(std::uint64_t k, int bits) noexcept {
    return static_cast<std::uint32_t>(k ^ (k >> bits));
  }
};

// Why not std::unordered_map: it chains through heap nodes, so every lookup is a dependent cache
// miss and every insert/erase allocates.
//
// This map is one preallocated array of 8-byte slots {fingerprint, store slot} at load factor
// <= 0.5. It does not store the 64-bit key. A fingerprint match is confirmed against the order record
// through the caller's key_of(slot), and every operation that looks up an order goes on to touch that
// record anyway, so the check adds almost no memory traffic. Halving the slot from 16 to 8 bytes
// halves the index's cache footprint; profiling showed the index was the largest source of cache
// misses in the engine. Deletion shifts later entries back instead of leaving tombstones: order flow
// is cancel-heavy, and tombstones would lengthen probe sequences for as long as the process runs.
// Insertion keeps Robin Hood order (within a run, entries sit in order of home slot), which lets a
// deletion stop at the first entry in its home slot and an absent-key lookup stop early; the probe
// distance is recomputed from the fingerprint, so the slot stays 8 bytes.
template <class Hash = LocalityHash>
class OrderIndex {
 public:
  explicit OrderIndex(std::uint32_t max_entries)
      : mask_(static_cast<std::uint32_t>(std::bit_ceil(std::max<std::uint64_t>(2ull * max_entries, 16)) - 1)),
        bits_(std::countr_zero(mask_ + 1ull)),
        slots_(mask_ + 1ull) {
    for (std::size_t i = 0; i <= mask_; ++i) slots_[i] = Slot{0, kNil};
  }

  // Returns the store slot holding `key`, or kNil. key_of(slot) must return the id stored there.
  template <class KeyOf>
  EXSIM_ALWAYS_INLINE std::uint32_t find(OrderId key, const KeyOf& key_of) const noexcept {
    const std::uint32_t fp = Hash::fingerprint(key, bits_);
    for (std::uint32_t i = fp & mask_, d = 0;; i = (i + 1) & mask_, ++d) {
      const Slot s = slots_[i];
      // Robin Hood order: had the key been here, it would sit before any entry closer to its own home.
      if (s.value == kNil || dist(i, s.fp) < d) return kNil;
      if (s.fp == fp && key_of(s.value) == key) return s.value;
    }
  }

  // Precondition: key is absent and size() < max_entries.
  EXSIM_ALWAYS_INLINE void insert(OrderId key, std::uint32_t value) noexcept {
    Slot cur{Hash::fingerprint(key, bits_), value};
    for (std::uint32_t i = cur.fp & mask_, d = 0;; i = (i + 1) & mask_, ++d) {
      if (slots_[i].value == kNil) {
        slots_[i] = cur;
        break;
      }
      // Robin Hood: the entry further from its home keeps the slot; the displaced one probes on.
      if (const std::uint32_t sd = dist(i, slots_[i].fp); sd < d) {
        std::swap(cur, slots_[i]);
        d = sd;
      }
    }
    ++size_;
  }

  // Removes the entry for (key, value) when the caller already knows the store slot (e.g. a maker
  // filled during a sweep). Needs no key verification: store slots are unique.
  EXSIM_ALWAYS_INLINE void erase_known(OrderId key, std::uint32_t value) noexcept {
    const std::uint32_t fp = Hash::fingerprint(key, bits_);
    std::uint32_t i = fp & mask_;
    while (slots_[i].value != value) i = (i + 1) & mask_;
    remove_at(i);
  }

  // Looks up and removes key; returns its store slot, or kNil if absent.
  template <class KeyOf>
  EXSIM_ALWAYS_INLINE std::uint32_t erase(OrderId key, const KeyOf& key_of) noexcept {
    const std::uint32_t fp = Hash::fingerprint(key, bits_);
    for (std::uint32_t i = fp & mask_, d = 0;; i = (i + 1) & mask_, ++d) {
      const Slot s = slots_[i];
      if (s.value == kNil || dist(i, s.fp) < d) return kNil;
      if (s.fp == fp && key_of(s.value) == key) {
        remove_at(i);
        return s.value;
      }
    }
  }

  std::size_t size() const noexcept { return size_; }
  std::size_t slot_count() const noexcept { return mask_ + 1ull; }

  // The Robin Hood invariant, for tests: along a run of occupied slots, the distance from home grows by at
  // most one per slot (equivalently, entries appear in order of home slot).
  bool robin_hood_ordered() const noexcept {
    for (std::uint32_t i = 0; i <= mask_; ++i) {
      const std::uint32_t p = (i - 1) & mask_;
      if (slots_[i].value == kNil) continue;
      const std::uint32_t d = dist(i, slots_[i].fp);
      if (d > 0 && (slots_[p].value == kNil || d > dist(p, slots_[p].fp) + 1)) return false;
    }
    return true;
  }

 private:
  struct Slot {
    std::uint32_t fp;
    std::uint32_t value;  // kNil marks an empty slot
  };
  static_assert(sizeof(Slot) == 8);

  // Probe distance of the entry at slot i from its home slot.
  EXSIM_ALWAYS_INLINE std::uint32_t dist(std::uint32_t i, std::uint32_t fp) const noexcept { return (i - fp) & mask_; }

  // Backward-shift deletion. Thanks to the Robin Hood order, the shift stops at the first entry that sits in
  // its home slot: nothing after it can belong earlier. Plain linear probing would have to scan on to the
  // next empty slot, which is the whole run when live ids are consecutive (the common case with sequential
  // exchange ids): 40,000 resting orders made every cancel scan tens of thousands of slots.
  EXSIM_ALWAYS_INLINE void remove_at(std::uint32_t hole) noexcept {
    for (std::uint32_t j = (hole + 1) & mask_; slots_[j].value != kNil && dist(j, slots_[j].fp) != 0;
         j = (j + 1) & mask_) {
      slots_[hole] = slots_[j];
      hole = j;
    }
    slots_[hole].value = kNil;
    --size_;
  }

  std::uint32_t mask_;
  int bits_;
  PageArray<Slot> slots_;
  std::size_t size_ = 0;
};

}  // namespace exsim
