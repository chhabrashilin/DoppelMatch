// Structure-aware differential fuzzing of the matching engine (libFuzzer).
//
// The fuzzer's bytes are decoded into a stream of commands on a tiny book (a 64-tick band, capacity 24, three
// owners), so that every edge is close: band edges, a full store, empty sides, self-trade prevention in every mode.
// Three implementations receive the same stream: the production AoS book, the hybrid layout, and the naive
// std::map reference. Any difference in their events, in their resting books, or a failed structural audit
// aborts, and libFuzzer keeps the input as a reproducer. Coverage guidance steers towards inputs that reach new
// branches, which random differential testing (tests/test_differential.cpp) finds only by luck.
//
//   cmake -DEXSIM_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++ ...; build/fuzz_book -max_total_time=60 corpus/book

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <vector>

#include "exsim/matching_engine.hpp"
#include "exsim/reference_book.hpp"

using namespace exsim;

namespace {

struct Recorder {
  std::vector<Event> ev;
  void on_event(const Event& e) { ev.push_back(e); }
};

class Bytes {
 public:
  Bytes(const std::uint8_t* d, std::size_t n) : d_(d), n_(n) {}
  bool empty() const { return i_ >= n_; }
  std::uint8_t u8() { return i_ < n_ ? d_[i_++] : 0; }

 private:
  const std::uint8_t* d_;
  std::size_t n_, i_ = 0;
};

template <class Book>
std::vector<std::tuple<Price, OrderId, Qty>> resting(const Book& b, Side s) {
  std::vector<std::tuple<Price, OrderId, Qty>> v;
  b.for_each_order(s, [&](Price p, OrderId id, Qty q) { v.emplace_back(p, id, q); });
  return v;
}

[[noreturn]] void fail(const char* what, std::size_t step) {
  std::fprintf(stderr, "fuzz_book: %s at command %zu\n", what, step);
  std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  Bytes in(data, size);
  BookConfig cfg;
  cfg.min_price = 100, cfg.num_levels = 64, cfg.max_orders = 24;
  cfg.stp = static_cast<Stp>(in.u8() & 3);
  AosBook aos(0, cfg);
  HybridBook hyb(0, cfg);
  ReferenceBook ref(0, cfg);

  for (std::size_t step = 0; !in.empty() && step < 4096; ++step) {
    const std::uint8_t op = in.u8();
    Command c{};
    c.order_id = 1 + in.u8() % 48;  // few ids: duplicates, cancels of live and dead orders
    c.price = cfg.min_price - 2 + in.u8() % 68;  // may fall just outside the band
    c.qty = in.u8() % 12;  // includes 0 (rejected)
    c.owner = 1 + in.u8() % 3;
    c.side = (op & 1) ? Side::Sell : Side::Buy;
    switch ((op >> 1) % 8) {
      case 0: case 1: case 2: case 3:
        c.type = MsgType::NewOrder;
        c.ord_type = ((op >> 4) & 7) == 0 ? OrdType::Market : OrdType::Limit;
        c.tif = static_cast<Tif>(((op >> 4) & 7) % 3);
        c.flags = static_cast<std::uint8_t>(((op >> 7) & 1) ? kFlagPostOnly : 0);
        c.flags |= stp_flag(static_cast<Stp>(in.u8() & 3));
        break;
      case 4: case 5: c.type = MsgType::Cancel; break;
      default: c.type = MsgType::Modify; break;
    }
    Recorder a, h, r;
    auto run = [&](auto& book, Recorder& rec) {
      switch (c.type) {
        case MsgType::NewOrder: book.add(c, rec); break;
        case MsgType::Cancel: book.cancel(c, rec); break;
        case MsgType::Modify: book.modify(c, rec); break;
      }
    };
    run(aos, a), run(hyb, h), run(ref, r);
    if (a.ev.size() != r.ev.size() || (!a.ev.empty() && std::memcmp(a.ev.data(), r.ev.data(), a.ev.size() * sizeof(Event)) != 0))
      fail("aos and reference emitted different events", step);
    if (h.ev.size() != r.ev.size() || (!h.ev.empty() && std::memcmp(h.ev.data(), r.ev.data(), h.ev.size() * sizeof(Event)) != 0))
      fail("hybrid and reference emitted different events", step);
    if ((step & 15) == 15 || in.empty()) {
      for (Side s : {Side::Buy, Side::Sell})
        if (resting(aos, s) != resting(ref, s) || resting(hyb, s) != resting(ref, s)) fail("resting books differ", step);
      if (!aos.check_invariants() || !hyb.check_invariants() || !ref.check_invariants()) fail("invariant audit failed", step);
    }
  }
  return 0;
}
