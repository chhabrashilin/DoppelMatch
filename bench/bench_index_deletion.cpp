// Deletion cost in the order-id index: plain linear probing versus Robin Hood order (order_index.hpp).
//
// Plain linear probing (Knuth's Algorithm R) must scan from the deleted slot to the next EMPTY slot, because
// any later entry in the run might belong earlier. With sequential order ids (exactly what the gateway now
// assigns) the live orders fill one contiguous run of the table, so a cancel scans the rest of the run.
// Robin Hood order lets the scan stop at the first entry that sits in its home slot.
//
// Found by scripts/e2e_sessions.py: its "harmless" baseline of 40,000 resting orders with consecutive ids
// spent more CPU in the index than in everything else combined.
//
//   exsim_bench_index [--live 40000]

#include <chrono>
#include <cstdio>
#include <vector>

#include "../tools/args.hpp"
#include "exsim/order_index.hpp"
#include "exsim/workload.hpp"

using namespace exsim;

namespace {

// The previous implementation, verbatim apart from the name: linear probing, backward shift to the next empty.
class LinearProbingIndex {
 public:
  explicit LinearProbingIndex(std::uint32_t max_entries)
      : mask_(static_cast<std::uint32_t>(std::bit_ceil(std::max<std::uint64_t>(2ull * max_entries, 16)) - 1)),
        bits_(std::countr_zero(mask_ + 1ull)),
        slots_(mask_ + 1ull, Slot{0, kNil}) {}
  template <class KeyOf>
  std::uint32_t erase(OrderId key, const KeyOf& key_of) {
    const std::uint32_t fp = LocalityHash::fingerprint(key, bits_);
    for (std::uint32_t i = fp & mask_;; i = (i + 1) & mask_) {
      const Slot s = slots_[i];
      if (s.value == kNil) return kNil;
      if (s.fp == fp && key_of(s.value) == key) {
        remove_at(i);
        return s.value;
      }
    }
  }
  void insert(OrderId key, std::uint32_t value) {
    const std::uint32_t fp = LocalityHash::fingerprint(key, bits_);
    std::uint32_t i = fp & mask_;
    while (slots_[i].value != kNil) i = (i + 1) & mask_;
    slots_[i] = Slot{fp, value};
  }

 private:
  struct Slot {
    std::uint32_t fp, value;
  };
  void remove_at(std::uint32_t hole) {
    for (std::uint32_t j = (hole + 1) & mask_; slots_[j].value != kNil; j = (j + 1) & mask_) {
      const std::uint32_t home = slots_[j].fp & mask_;
      if (((j - home) & mask_) >= ((j - hole) & mask_)) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole].value = kNil;
  }
  std::uint32_t mask_;
  int bits_;
  std::vector<Slot> slots_;
};

// Inserts `live` consecutive ids, then erases them in the given order; returns ns per erase (best of reps).
template <class Index>
double ns_per_erase(std::uint32_t live, const std::vector<std::uint32_t>& order, std::uint32_t capacity) {
  double best = 1e30;
  for (int rep = 0; rep < 5; ++rep) {
    Index idx(capacity);
    for (std::uint32_t i = 0; i < live; ++i) idx.insert(i + 1, i);
    auto key_of = [](std::uint32_t s) { return OrderId{s + 1}; };
    std::uint64_t sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const std::uint32_t i : order) sink += idx.erase(i + 1, key_of);
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
    if (sink == 42) std::printf(" ");
    best = std::min(best, ns / static_cast<double>(order.size()));
  }
  return best;
}

}  // namespace

int main(int argc, char** argv) {
  const tools::Args args(argc, argv);
  const auto capacity = static_cast<std::uint32_t>(args.u64("capacity", 1u << 18));  // the engine's default
  std::printf("id-index deletion, capacity %u (ns per erase, best of 5)\n\n", capacity);
  std::printf("| live orders | erase order | linear probing | Robin Hood | speedup |\n");
  std::printf("|------------:|-------------|---------------:|-----------:|--------:|\n");
  for (const std::uint32_t live : {1000u, 4000u, 40000u}) {
    std::vector<std::uint32_t> fifo(live), rnd(live);
    for (std::uint32_t i = 0; i < live; ++i) fifo[i] = rnd[i] = i;
    Rng rng(5);
    for (std::uint32_t i = live - 1; i > 0; --i) std::swap(rnd[i], rnd[rng.below(i + 1)]);
    for (const auto& [name, order] : {std::pair{"oldest first", &fifo}, std::pair{"random", &rnd}}) {
      const double a = ns_per_erase<LinearProbingIndex>(live, *order, capacity);
      const double b = ns_per_erase<OrderIndex<LocalityHash>>(live, *order, capacity);
      std::printf("| %11u | %-11s | %14.1f | %10.1f | %6.0fx |\n", live, name, a, b, a / b);
    }
  }
  return 0;
}
