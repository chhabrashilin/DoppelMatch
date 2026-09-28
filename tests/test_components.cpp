// Data structures and infrastructure, each checked against a trusted model.

#include <atomic>
#include <bit>
#include <cmath>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "exsim/client_ids.hpp"
#include "exsim/latency_histogram.hpp"
#include "exsim/order_index.hpp"
#include "exsim/price_bitmap.hpp"
#include "exsim/protocol.hpp"
#include "exsim/spsc_queue.hpp"
#include "exsim/wire_file.hpp"
#include "exsim/workload.hpp"
#include "test_framework.hpp"

using namespace exsim;

TEST(bitmap_matches_std_set) {
  for (std::uint32_t n : {64u, 4096u, 1u << 16}) {
    PriceBitmap bm(n);
    std::set<std::int32_t> model;
    Rng rng(n);
    const auto sn = static_cast<std::int32_t>(n);
    for (int step = 0; step < 50'000; ++step) {
      const auto i = static_cast<std::int32_t>(rng.below(n));
      if (rng.chance(0.5)) {
        bm.set(i);
        model.insert(i);
      } else {
        bm.clear(i);
        model.erase(i);
      }
      const auto q = static_cast<std::int32_t>(rng.below(n + 2)) - 1;  // include out-of-range probes
      auto it = model.lower_bound(q);
      CHECK_EQ(bm.find_next(q), it == model.end() ? sn : *it);
      auto jt = model.upper_bound(q);
      CHECK_EQ(bm.find_prev(q), jt == model.begin() ? -1 : *std::prev(jt));
    }
  }
}

TEST(bitmap_rejects_bad_sizes) {
  bool threw = false;
  try {
    PriceBitmap bm(100);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

template <class Hash>
void index_matches_unordered_map() {
  // Small table + clustered keys: long probe chains and wraparound, the backward-shift worst case.
  // The index stores only fingerprints, so a side table plays the order store: owner[slot] = key.
  OrderIndex<Hash> idx(64);
  std::unordered_map<OrderId, std::uint32_t> model;
  std::vector<OrderId> owner(1'000'000, 0);
  auto key_of = [&](std::uint32_t slot) { return owner[slot]; };
  Rng rng(7);
  std::uint32_t next_slot = 0;
  for (int step = 0; step < 400'000; ++step) {
    const OrderId key = 1 + rng.below(200);
    if (rng.chance(0.5) && model.size() < 64 && model.count(key) == 0) {
      const std::uint32_t v = next_slot++ % 1'000'000;
      owner[v] = key;
      idx.insert(key, v);
      model[key] = v;
    } else if (rng.chance(0.5)) {
      const auto it = model.find(key);
      const std::uint32_t got = idx.erase(key, key_of);
      CHECK_EQ(got, it == model.end() ? kNil : it->second);
      if (it != model.end()) model.erase(it);
    } else if (!model.empty()) {  // erase by known slot, as the matcher does for filled makers
      const auto it = model.begin();
      idx.erase_known(it->first, it->second);
      model.erase(it);
    }
    CHECK_EQ(idx.size(), model.size());
    CHECK(idx.robin_hood_ordered());
    const OrderId probe = 1 + rng.below(200);
    const auto it = model.find(probe);
    CHECK_EQ(idx.find(probe, key_of), it == model.end() ? kNil : it->second);
  }
}
TEST(order_index_scatter_matches_unordered_map) { index_matches_unordered_map<ScatterHash>(); }
TEST(order_index_locality_matches_unordered_map) { index_matches_unordered_map<LocalityHash>(); }

// Keys crafted against the fold in LocalityHash: with bits = log2(table size), key = (i << bits) | (i ^ C)
// has fingerprint (key ^ (key >> bits)) = C in the low bits for every i, so all keys share ONE home slot.
// This is the documented worst case of the locality hash. Correctness must survive it; only speed suffers.
static std::uint64_t colliding_key(std::uint64_t i, int bits, std::uint64_t c) { return (i << bits) | (i ^ c); }

TEST(order_index_locality_hash_stays_correct_under_a_worst_case_collision_attack) {
  OrderIndex<LocalityHash> idx(512);
  const int bits = std::countr_zero(idx.slot_count());
  const std::uint64_t C = 77;
  auto key_of = [&](std::uint32_t slot) { return colliding_key(slot + 1, bits, C); };
  for (std::uint32_t i = 0; i < 512; ++i) idx.insert(colliding_key(i + 1, bits, C), i);
  // every key really does hash to the same home slot: consecutive inserts probe a chain of growing length
  for (std::uint32_t i = 0; i < 512; ++i) CHECK_EQ(idx.find(colliding_key(i + 1, bits, C), key_of), i);
  for (std::uint32_t i = 0; i < 512; i += 2) CHECK_EQ(idx.erase(colliding_key(i + 1, bits, C), key_of), i);
  for (std::uint32_t i = 1; i < 512; i += 2) CHECK_EQ(idx.find(colliding_key(i + 1, bits, C), key_of), i);
  CHECK_EQ(idx.find(colliding_key(9999, bits, C), key_of), kNil);
  CHECK_EQ(idx.size(), 256u);
  CHECK(idx.robin_hood_ordered());
}

TEST(spsc_single_thread_semantics) {
  SpscQueue<int> q(4);
  CHECK_EQ(q.capacity(), 4u);
  CHECK(q.front() == nullptr);
  for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
  CHECK(!q.try_push(99));  // full
  int v = -1;
  CHECK(q.try_pop(v));
  CHECK_EQ(v, 0);
  CHECK(q.try_push(4));  // wraps
  for (int want = 1; want <= 4; ++want) {
    REQUIRE(q.try_pop(v));
    CHECK_EQ(v, want);
  }
  CHECK(!q.try_pop(v));
}

TEST(spsc_two_threads_preserve_order) {
  constexpr std::uint64_t kItems = 5'000'000;
  SpscQueue<std::uint64_t> q(1024);
  std::atomic<bool> ok{true};
  std::thread consumer([&] {
    std::uint64_t expect = 0;
    while (expect < kItems) {
      if (std::uint64_t* p = q.front()) {
        if (*p != expect) ok = false;
        q.pop();
        ++expect;
      }
    }
  });
  for (std::uint64_t i = 0; i < kItems; ++i)
    while (!q.try_push(i)) {
    }
  consumer.join();
  CHECK(ok.load());
  CHECK(q.front() == nullptr);
}

TEST(histogram_percentiles_within_bucket_error) {
  LatencyHistogram h;
  std::vector<std::uint64_t> values;
  Rng rng(3);
  for (int i = 0; i < 200'000; ++i) {
    const auto v = static_cast<std::uint64_t>(std::exp(rng.uniform() * 14.0));  // 1 .. ~1.2M, log-uniform
    values.push_back(v);
    h.record(v);
  }
  std::sort(values.begin(), values.end());
  for (double p : {50.0, 90.0, 99.0, 99.9, 99.99}) {
    const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(values.size())));
    const std::uint64_t exact = values[rank - 1];
    const std::uint64_t got = h.percentile(p);
    CHECK(got >= exact);                                           // reports bucket upper bound
    CHECK(static_cast<double>(got) <= static_cast<double>(exact) * (1.0 + 1.0 / 32) + 1);  // within one bucket
  }
  CHECK_EQ(h.max(), values.back());
  CHECK_EQ(h.min(), values.front());
  CHECK_EQ(h.count(), values.size());
}

TEST(histogram_bucket_boundaries_are_contiguous) {
  for (std::uint64_t v = 0; v < 100'000; ++v) {
    const auto i = LatencyHistogram::index(v);
    CHECK(v <= LatencyHistogram::upper_bound(i));
    if (i > 0) CHECK(v > LatencyHistogram::upper_bound(i - 1));
  }
  CHECK(LatencyHistogram::index(~0ull) < LatencyHistogram::kBuckets);
}

TEST(protocol_roundtrip_every_field) {
  WorkloadConfig wc;
  wc.messages = 100'000;
  const auto cmds = generate_workload(wc);
  DecodeStats st;
  const auto back = decode_stream(encode_stream(cmds), &st);
  CHECK_EQ(st.ok, cmds.size());
  CHECK_EQ(st.malformed, 0u);
  CHECK(!st.truncated);
  REQUIRE(back.size() == cmds.size());
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    // Cancel carries only id + symbol on the wire; compare the fields each type actually transmits.
    const Command &a = cmds[i], &b = back[i];
    bool same = a.type == b.type && a.order_id == b.order_id && a.symbol == b.symbol && a.seq == b.seq;
    if (a.type != MsgType::Cancel) same = same && a.price == b.price && a.qty == b.qty;
    if (a.type == MsgType::NewOrder)
      same = same && a.side == b.side && a.ord_type == b.ord_type && a.tif == b.tif && a.flags == b.flags &&
             a.owner == b.owner;
    if (!same) {
      CHECK(same);
      break;
    }
  }
}

TEST(protocol_rejects_malformed_input) {
  Command c{};
  c.type = MsgType::NewOrder;
  c.order_id = 5, c.qty = 1, c.price = 100;
  std::byte buf[wire::kMaxMessageSize];
  const std::size_t n = wire::encode(c, buf);
  Command out;

  CHECK(wire::decode(buf, n - 1, out).status == wire::DecodeStatus::Incomplete);
  CHECK(wire::decode(buf, 3, out).status == wire::DecodeStatus::Incomplete);

  auto corrupt = [&](std::size_t offset, std::uint8_t value) {
    std::byte copy[wire::kMaxMessageSize];
    std::memcpy(copy, buf, n);
    copy[offset] = std::byte{value};
    return wire::decode(copy, n, out);
  };
  auto r = corrupt(2, 99);  // unknown type: skippable
  CHECK(r.status == wire::DecodeStatus::Malformed && r.consumed == n);
  r = corrupt(3, 7);  // bad version
  CHECK(r.status == wire::DecodeStatus::Malformed && r.consumed == n);
  r = corrupt(offsetof(wire::NewOrder, side), 2);  // side out of range
  CHECK(r.status == wire::DecodeStatus::Malformed && r.consumed == n);
  r = corrupt(offsetof(wire::NewOrder, tif), 3);
  CHECK(r.status == wire::DecodeStatus::Malformed);
  r = corrupt(0, 4);  // length smaller than a header: framing is lost
  CHECK(r.status == wire::DecodeStatus::Malformed && r.consumed == 0);

  // A malformed message in the middle of a stream is skipped; its neighbours still decode.
  std::byte bad[wire::kMaxMessageSize];
  std::memcpy(bad, buf, n);
  bad[2] = std::byte{42};
  std::vector<std::byte> stream(buf, buf + n);
  stream.insert(stream.end(), bad, bad + n);
  stream.insert(stream.end(), buf, buf + n);
  DecodeStats st;
  CHECK_EQ(decode_stream(stream, &st).size(), 2u);
  CHECK_EQ(st.malformed, 1u);
  CHECK(!st.truncated);
}

TEST(siphash_matches_the_reference_vectors) {
  // Key 00 01 .. 0f; the paper's worked example (15-byte message 00 .. 0e) and the empty message.
  const std::uint64_t k0 = 0x0706050403020100ull, k1 = 0x0f0e0d0c0b0a0908ull;
  unsigned char msg[15];
  for (int i = 0; i < 15; ++i) msg[i] = static_cast<unsigned char>(i);
  CHECK_EQ((siphash<2, 4>(k0, k1, msg, 15)), 0xa129ca6149be45e5ull);
  CHECK_EQ((siphash<2, 4>(k0, k1, msg, 0)), 0x726fdb47dd0e0e31ull);
}

TEST(client_ids_map_to_sequential_exchange_ids) {
  ClientIdMap ids(100);
  using K = ClientIdMap::Key;
  const OrderId a = ids.for_new(K{7, 0, 1});
  const OrderId b = ids.for_new(K{7, 0, 2});  // same client id, other owner: a different order
  const OrderId c = ids.for_new(K{7, 1, 1});  // same client id, other symbol: a different order
  CHECK_EQ(a, OrderId{100});
  CHECK_EQ(b, OrderId{101});
  CHECK_EQ(c, OrderId{102});
  CHECK_EQ(ids.for_new(K{7, 0, 1}), a);       // live duplicate: the engine must see the same id to reject it
  CHECK_EQ(ids.find(K{7, 0, 1}), a);
  CHECK_EQ(ids.find(K{8, 0, 1}), OrderId{0}); // unknown: 0, which is never assigned
  CHECK_EQ(ids.for_new(K{0, 0, 1}), OrderId{0});
  CHECK_EQ(ids.to_client(a, 1), OrderId{7});
  CHECK_EQ(ids.to_client(a, 2), OrderId{0});  // someone else's order is anonymous
  CHECK_EQ(ids.owner_of(b), OwnerId{2});
  ids.retire(a);
  CHECK_EQ(ids.find(K{7, 0, 1}), OrderId{0});
  CHECK_EQ(ids.for_new(K{7, 0, 1}), OrderId{103});  // a reused client id is a new order
  CHECK_EQ(ids.live(), std::size_t{3});
}
