// The real C++ matching engine compiled to WebAssembly for the browser UI.
//
// This is the same OrderBook code that the tests, benchmarks and gateway run: the UI is not a
// re-implementation. A small C API keeps the JS side simple: submit a command, read back the events
// it produced, query the depth ladder.
//
// Build: scripts/build_wasm.sh   (Emscripten; produces ui/engine.js with the wasm embedded)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "exsim/matching_engine.hpp"
#include "exsim/sinks.hpp"
#include "exsim/workload.hpp"

#include <emscripten/emscripten.h>

using namespace exsim;

namespace {

using Book = DefaultBook;
std::unique_ptr<Book> g_book;
VectorSink g_sink;  // events of the last command; reused, so steady-state submits do not reallocate

struct Level { std::int32_t px; std::uint32_t qty; std::uint32_t orders; };
std::vector<Level> g_depth;

struct Resting { std::uint64_t id; std::uint32_t qty; std::uint32_t owner; };
std::vector<Resting> g_queue;

constexpr Price kMinPrice = 0;
constexpr std::uint32_t kLevels = 4096;

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void ex_init() {
  BookConfig cfg;
  cfg.min_price = kMinPrice, cfg.num_levels = kLevels, cfg.max_orders = 1u << 16, cfg.stp = Stp::None;
  g_book = std::make_unique<Book>(0, cfg);
  g_sink.events.clear();
}

// type: 1 new, 2 cancel, 3 modify. side: 0 buy, 1 sell. ord_type: 0 limit, 1 market. tif: 0 day, 1 ioc, 2 fok.
// Returns the number of events produced; read them at ex_events().
EMSCRIPTEN_KEEPALIVE int ex_submit(int type, double id, int side, int ord_type, int tif, int post_only, int price,
                                   unsigned qty, unsigned owner) {
  Command c{};
  c.type = static_cast<MsgType>(type);
  c.order_id = static_cast<OrderId>(id);
  c.side = static_cast<Side>(side);
  c.ord_type = static_cast<OrdType>(ord_type);
  c.tif = static_cast<Tif>(tif);
  c.flags = post_only ? kFlagPostOnly : 0;
  c.price = price;
  c.qty = qty;
  c.owner = owner;
  g_sink.events.clear();
  switch (c.type) {
    case MsgType::NewOrder: g_book->add(c, g_sink); break;
    case MsgType::Cancel: g_book->cancel(c, g_sink); break;
    case MsgType::Modify: g_book->modify(c, g_sink); break;
  }
  return static_cast<int>(g_sink.events.size());
}

// Event layout (48 bytes): u64 order_id @0, u64 maker_id @8, i64 price @16, u64 qty @24, u64 leaves @32,
// u32 symbol @40, u8 type @44, u8 reason @45, u8 side @46, u8 request @47.
EMSCRIPTEN_KEEPALIVE const Event* ex_events() { return g_sink.events.data(); }
EMSCRIPTEN_KEEPALIVE int ex_event_size() { return static_cast<int>(sizeof(Event)); }

EMSCRIPTEN_KEEPALIVE int ex_best_bid() { auto b = g_book->best_bid(); return b ? static_cast<int>(*b) : -1; }
EMSCRIPTEN_KEEPALIVE int ex_best_ask() { auto a = g_book->best_ask(); return a ? static_cast<int>(*a) : -1; }
EMSCRIPTEN_KEEPALIVE int ex_order_count() { return static_cast<int>(g_book->order_count()); }
EMSCRIPTEN_KEEPALIVE int ex_check_invariants() { return g_book->check_invariants() ? 1 : 0; }

// Aggregated depth of one side, best first. Returns the level count; read at ex_depth_ptr() as
// {i32 price, u32 qty, u32 orders} triples.
EMSCRIPTEN_KEEPALIVE int ex_depth(int side, int max_levels) {
  g_depth.clear();
  g_book->for_each_order(static_cast<Side>(side), [&](Price p, OrderId, Qty q) {
    if (!g_depth.empty() && g_depth.back().px == p) {
      g_depth.back().qty += static_cast<std::uint32_t>(q), ++g_depth.back().orders;  // UI sizes fit in 32 bits
    } else if (static_cast<int>(g_depth.size()) < max_levels) {
      g_depth.push_back({static_cast<std::int32_t>(p), static_cast<std::uint32_t>(q), 1});
    }
  });
  return static_cast<int>(g_depth.size());
}
EMSCRIPTEN_KEEPALIVE const Level* ex_depth_ptr() { return g_depth.data(); }

// Orders resting at one price, in queue (priority) order. Read at ex_queue_ptr() as {u64 id, u32 qty, u32 owner}.
EMSCRIPTEN_KEEPALIVE int ex_queue(int side, int price) {
  g_queue.clear();
  g_book->for_each_order(static_cast<Side>(side), [&](Price p, OrderId id, Qty q) {
    if (p == price) {
      const auto v = g_book->find_order(id);
      g_queue.push_back({id, static_cast<std::uint32_t>(q), v ? v->owner : 0});
    }
  });
  return static_cast<int>(g_queue.size());
}
EMSCRIPTEN_KEEPALIVE const Resting* ex_queue_ptr() { return g_queue.data(); }

// Live order lookup: returns the remaining quantity (0 if the order is gone); price/side/owner via the out params.
EMSCRIPTEN_KEEPALIVE unsigned ex_find(double id, int* px, int* side, unsigned* owner) {
  const auto v = g_book->find_order(static_cast<OrderId>(id));
  if (!v) return 0;
  *px = static_cast<int>(v->price), *side = v->side == Side::Buy ? 0 : 1, *owner = v->owner;
  return static_cast<unsigned>(v->qty);
}

// Self-contained throughput test on a private book: `n` random orders from a fixed seed, measured
// inside the module. Returns messages per second.
EMSCRIPTEN_KEEPALIVE double ex_bench(int n, int seed) {
  BookConfig cfg;
  cfg.min_price = 0, cfg.num_levels = kLevels, cfg.max_orders = 1u << 16;
  Book book(0, cfg);
  struct Count { std::uint64_t trades = 0; void on_event(const Event& e) { trades += e.type == EventType::Trade; } } sink;
  Rng rng(static_cast<std::uint64_t>(seed));
  Price mid = 2000;
  std::vector<OrderId> live;
  OrderId next = 1;
  live.reserve(8192);
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) {
    if (rng.chance(0.1)) mid += rng.chance(0.5) ? 1 : -1;
    Command c{};
    if (live.size() > 3000 && rng.chance(0.4)) {
      const std::size_t k = rng.below(live.size());
      c.type = MsgType::Cancel, c.order_id = live[k];
      live[k] = live.back(), live.pop_back();
      book.cancel(c, sink);
      continue;
    }
    c.type = MsgType::NewOrder, c.order_id = next++;
    c.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
    const bool cross = rng.chance(0.1);
    const Price d = static_cast<Price>(1 + rng.below(8));
    c.price = c.side == Side::Buy ? mid + (cross ? d : -d) : mid - (cross ? d : -d);
    c.qty = static_cast<Qty>(50 * (1 + rng.below(6)));
    book.add(c, sink);
    if (book.contains(c.order_id)) live.push_back(c.order_id);
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return secs > 0 ? n / secs : 0;
}

}  // extern "C"
