// Randomized differential testing: the strongest correctness evidence in the repository.
//
// Identical random command streams go to the naive ReferenceBook and to every optimized variant.
// After each command, every implementation must have emitted byte-identical events. Periodically
// the full resting book (every order, in priority order) is compared as well, and each
// implementation's structural invariants are audited.
//
// The generator is tuned to hit edge cases often: a 128-tick band (levels empty and refill
// constantly, and orders hit the band edges), capacity 200 (BookFull triggers), four owners (STP
// triggers constantly), a drifting mid, every order type, cancels and modifies of live, dead and
// never-seen ids, duplicate ids, and invalid fields.

#include <cstdio>
#include <tuple>

#include "exsim/wire_file.hpp"
#include "exsim/workload.hpp"
#include "helpers.hpp"

using namespace exsim;
using namespace exsim::test;

namespace {

struct Snapshot {
  std::vector<std::tuple<Price, OrderId, Qty>> bids, asks;
  bool operator==(const Snapshot&) const = default;
};

// Bounded walk: a corrupted FIFO can contain a cycle, which must fail the test, not hang it.
template <class Book>
Snapshot snapshot(const Book& b) {
  Snapshot s;
  std::size_t visited = 0;
  auto guard = [&] {
    if (++visited > 1'000'000) {
      report(__FILE__, __LINE__, "cycle detected while walking the book");
      throw Failure{};
    }
  };
  b.for_each_order(Side::Buy, [&](Price p, OrderId id, Qty q) { guard(), s.bids.emplace_back(p, id, q); });
  b.for_each_order(Side::Sell, [&](Price p, OrderId id, Qty q) { guard(), s.asks.emplace_back(p, id, q); });
  return s;
}

Command random_command(Rng& rng, Price& mid, OrderId& next_id, const BookConfig& cfg) {
  const Price lo = cfg.min_price, hi = cfg.min_price + static_cast<Price>(cfg.num_levels) - 1;
  if (rng.chance(0.05)) mid = std::clamp<Price>(mid + static_cast<Price>(rng.below(5)) - 2, lo + 5, hi - 5);
  auto recent_id = [&] { return next_id <= 1 ? OrderId{1} : next_id - 1 - rng.below(std::min<OrderId>(next_id - 1, 300)); };
  auto near_price = [&] { return mid + static_cast<Price>(rng.below(31)) - 15; };  // may fall outside the band

  const double r = rng.uniform();
  Command c{};
  if (r < 0.50) {
    c.type = MsgType::NewOrder;
    c.order_id = rng.chance(0.02) ? recent_id() : next_id++;  // occasional duplicate
    if (rng.chance(0.005)) c.order_id = 0;
    c.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
    c.price = near_price();
    c.qty = rng.chance(0.01) ? 0 : static_cast<Qty>(1 + rng.below(20));
    c.owner = static_cast<OwnerId>(1 + rng.below(4));
    const double t = rng.uniform();
    if (t < 0.05) c.ord_type = OrdType::Market;
    if (t >= 0.05 && t < 0.13) c.tif = Tif::Ioc;
    if (t >= 0.13 && t < 0.17) c.tif = Tif::Fok;
    if (t >= 0.17 && t < 0.22) c.flags = kFlagPostOnly;
    if (rng.chance(0.3)) c.flags |= stp_flag(static_cast<Stp>(rng.below(4)));  // per-order STP override
  } else if (r < 0.80) {
    c.type = MsgType::Cancel;
    c.order_id = rng.chance(0.9) ? recent_id() : rng.below(next_id + 10);
  } else {
    c.type = MsgType::Modify;
    c.order_id = rng.chance(0.95) ? recent_id() : rng.below(next_id + 10);
    c.price = near_price();
    c.qty = rng.chance(0.01) ? 0 : static_cast<Qty>(1 + rng.below(25));
  }
  return c;
}

template <class Book>
void apply(Book& b, const Command& c, VectorSink& s) {
  s.events.clear();
  switch (c.type) {
    case MsgType::NewOrder: b.add(c, s); break;
    case MsgType::Cancel: b.cancel(c, s); break;
    case MsgType::Modify: b.modify(c, s); break;
  }
}

std::string describe(const Command& c) {
  char buf[160];
  std::snprintf(buf, sizeof buf, "type=%d id=%llu side=%d px=%lld qty=%llu ord=%d tif=%d flags=%d owner=%u",
                static_cast<int>(c.type), static_cast<unsigned long long>(c.order_id), static_cast<int>(c.side),
                static_cast<long long>(c.price), static_cast<unsigned long long>(c.qty), static_cast<int>(c.ord_type), static_cast<int>(c.tif), c.flags,
                c.owner);
  return buf;
}

// Runs one seed. Returns false (after reporting) at the first divergence.
bool run_seed(std::uint64_t seed, Stp stp, int ops, std::uint64_t& events_compared) {
  BookConfig cfg;
  cfg.min_price = 1000;
  cfg.num_levels = 128;
  cfg.max_orders = 200;
  cfg.stp = stp;
  ReferenceBook ref(0, cfg);
  AosScatterBook aos_scatter(0, cfg);
  AosBook aos(0, cfg);
  SoaBook soa(0, cfg);
  HybridBook hyb(0, cfg);
  VectorSink sr, sx, sa, ss, sh;
  Rng rng(seed * 1000003 + static_cast<std::uint64_t>(stp));
  Price mid = 1064;
  OrderId next_id = 1;

  for (int i = 0; i < ops; ++i) {
    const Command c = random_command(rng, mid, next_id, cfg);
    apply(ref, c, sr);
    apply(aos_scatter, c, sx);
    apply(aos, c, sa);
    apply(soa, c, ss);
    apply(hyb, c, sh);
    for (const VectorSink* other : {&sx, &sa, &ss, &sh}) {
      bool same = other->events.size() == sr.events.size();
      for (std::size_t k = 0; same && k < sr.events.size(); ++k) same = same_bytes(other->events[k], sr.events[k]);
      if (!same) {
        report(__FILE__, __LINE__,
               "divergence: seed " + std::to_string(seed) + " stp " + std::to_string(static_cast<int>(stp)) + " op " +
                   std::to_string(i) + ": " + describe(c) + " (ref emitted " + std::to_string(sr.events.size()) +
                   " events, impl " + std::to_string(other->events.size()) + ")");
        return false;
      }
    }
    events_compared += sr.events.size();
    if (i % 50 == 0 || i == ops - 1) {
      // Invariants first: check_invariants() is cycle-safe, so structural corruption is named precisely.
      if (!(ref.check_invariants() && aos_scatter.check_invariants() && aos.check_invariants() && soa.check_invariants() && hyb.check_invariants())) {
        report(__FILE__, __LINE__, "invariant violation: seed " + std::to_string(seed) + " op " + std::to_string(i));
        return false;
      }
      const Snapshot want = snapshot(ref);
      if (!(snapshot(aos_scatter) == want && snapshot(aos) == want && snapshot(soa) == want && snapshot(hyb) == want)) {
        report(__FILE__, __LINE__, "book snapshot divergence: seed " + std::to_string(seed) + " op " + std::to_string(i));
        return false;
      }
      CHECK_EQ(ref.best_bid_qty(), hyb.best_bid_qty());
      CHECK_EQ(ref.best_ask_qty(), hyb.best_ask_qty());
    }
  }
  return true;
}

}  // namespace

TEST(differential_random_streams_all_stp_policies) {
  std::uint64_t events = 0;
  int seeds = 0;
  for (Stp stp : {Stp::None, Stp::CancelResting, Stp::CancelIncoming, Stp::DecrementCancel}) {
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
      ++seeds;
      if (!run_seed(seed, stp, 10'000, events)) return;
    }
  }
  std::printf("      %d seeds x 10k ops, %llu events compared byte-for-byte across 5 implementations\n", seeds,
              static_cast<unsigned long long>(events));
  CHECK(events > 1'500'000);
}

TEST(realistic_workload_digests_agree_and_replay_is_deterministic) {
  WorkloadConfig wc;
  wc.messages = 400'000;
  wc.symbols = 4;
  const auto cmds = generate_workload(wc);
  auto digest_of = [&](auto tag, const std::vector<Command>& stream) {
    using Book = typename decltype(tag)::type;
    MatchingEngine<Book> engine(wc.symbols, wc.book);
    CountingSink sink;
    for (const Command& c : stream) engine.process(c, sink);
    return std::pair{sink.digest(), sink.count(EventType::Trade)};
  };
  const auto ref = digest_of(std::type_identity<ReferenceBook>{}, cmds);
  CHECK(ref.second > 10'000);  // the workload actually trades
  CHECK_EQ(digest_of(std::type_identity<AosScatterBook>{}, cmds).first, ref.first);
  CHECK_EQ(digest_of(std::type_identity<AosBook>{}, cmds).first, ref.first);
  CHECK_EQ(digest_of(std::type_identity<SoaBook>{}, cmds).first, ref.first);
  CHECK_EQ(digest_of(std::type_identity<HybridBook>{}, cmds).first, ref.first);
  CHECK_EQ(digest_of(std::type_identity<HybridBook>{}, cmds).first, ref.first);  // run-to-run determinism

  // Through the wire: encode -> decode -> replay yields the same events.
  const auto decoded = decode_stream(encode_stream(cmds));
  CHECK_EQ(digest_of(std::type_identity<HybridBook>{}, decoded).first, ref.first);

  // The generator itself is a pure function of its config.
  const auto again = generate_workload(wc);
  CHECK(again.size() == cmds.size() && std::memcmp(again.data(), cmds.data(), cmds.size() * sizeof(Command)) == 0);
}
