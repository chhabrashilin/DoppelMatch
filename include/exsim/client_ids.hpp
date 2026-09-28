// Exchange-assigned order ids: the gateway's translation between client order ids and engine order ids.
//
// Clients name their orders with their own ids (FIX ClOrdID). If those ids reached the engine unchanged,
// a participant could choose ids that collide in the engine's order index: LocalityHash is fast because it
// is simple, and simple means invertible (BENCHMARKS.md measures a 300-500x lookup slowdown for crafted
// keys). Real venues do not let clients choose the engine's keys. Neither does this gateway:
//
//   * Every new order gets the next exchange id (1, 2, 3, ...) BEFORE it is sequenced and journaled. The
//     engine, the journal and every replica only ever see exchange ids, which are sequential: exactly the
//     keys the locality hash is built for, and not chosen by anyone outside.
//   * The map from (owner, symbol, client id) to exchange id is the only structure keyed by client input,
//     so it is hashed with SipHash-1-3 under a random per-process key. Without the key an attacker cannot
//     predict which ids collide; this is the standard defence against hash flooding.
//   * Reports are translated back: an order's owner sees its own client id, and anyone else sees 0 (the
//     counterparty is anonymous, as on a real venue).
//
// A duplicate client id of a live order maps to that order's exchange id, so the engine itself rejects it
// as a duplicate, and the id of an order that is not live maps to 0, which is never assigned, so the engine
// rejects it as unknown. The engine's validation order is therefore unchanged, and a client's report stream
// is identical to what a local engine run on its own ids produces (exsim_client checks this).
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <random>
#include <unordered_map>

#include "exsim/common.hpp"

namespace exsim {

// SipHash (Aumasson and Bernstein, 2012) with C compression and D finalization rounds; SipHash-1-3 is the
// variant Rust and Python use for hash tables.
template <int C, int D>
std::uint64_t siphash(std::uint64_t k0, std::uint64_t k1, const void* data, std::size_t n) noexcept {
  std::uint64_t v0 = k0 ^ 0x736f6d6570736575ull, v1 = k1 ^ 0x646f72616e646f6dull;
  std::uint64_t v2 = k0 ^ 0x6c7967656e657261ull, v3 = k1 ^ 0x7465646279746573ull;
  auto round = [&] {
    v0 += v1, v1 = std::rotl(v1, 13), v1 ^= v0, v0 = std::rotl(v0, 32);
    v2 += v3, v3 = std::rotl(v3, 16), v3 ^= v2;
    v0 += v3, v3 = std::rotl(v3, 21), v3 ^= v0;
    v2 += v1, v1 = std::rotl(v1, 17), v1 ^= v2, v2 = std::rotl(v2, 32);
  };
  auto compress = [&](std::uint64_t m) {
    v3 ^= m;
    for (int i = 0; i < C; ++i) round();
    v0 ^= m;
  };
  const auto* p = static_cast<const unsigned char*>(data);
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    std::uint64_t m;
    std::memcpy(&m, p + i, 8);  // little endian, as the reference implementation reads it
    compress(m);
  }
  std::uint64_t last = static_cast<std::uint64_t>(n & 0xff) << 56;
  for (std::size_t j = 0; i + j < n; ++j) last |= static_cast<std::uint64_t>(p[i + j]) << (8 * j);
  compress(last);
  v2 ^= 0xff;
  for (int r = 0; r < D; ++r) round();
  return v0 ^ v1 ^ v2 ^ v3;
}

class ClientIdMap {
 public:
  struct Key {
    OrderId client_id;
    SymbolId symbol;
    OwnerId owner;
    bool operator==(const Key&) const = default;
  };
  static_assert(sizeof(Key) == 16);

  // `first_exchange_id` must exceed every exchange id already in the book (after a recovery or promotion).
  explicit ClientIdMap(OrderId first_exchange_id = 1, std::uint64_t k0 = random_key(), std::uint64_t k1 = random_key())
      : fwd_(1024, Hasher{k0, k1}), next_(first_exchange_id) {}

  // A new order: the live order's exchange id if this client id is live (the engine rejects the duplicate),
  // else a fresh one. Client id 0 stays 0 (the engine rejects it as invalid).
  OrderId for_new(const Key& k) {
    if (k.client_id == 0) return 0;
    auto [it, fresh] = fwd_.try_emplace(k, next_);
    if (fresh) rev_.emplace(next_++, k);
    return it->second;
  }
  // A cancel or modify: the exchange id of a live order, or 0 (never assigned, so the engine reports unknown).
  OrderId find(const Key& k) const {
    const auto it = fwd_.find(k);
    return it == fwd_.end() ? 0 : it->second;
  }
  // What `viewer` is shown for exchange id x: its own client id, or 0 for someone else's order.
  OrderId to_client(OrderId x, OwnerId viewer) const {
    const auto it = rev_.find(x);
    return it != rev_.end() && it->second.owner == viewer ? it->second.client_id : 0;
  }
  OwnerId owner_of(OrderId x) const {
    const auto it = rev_.find(x);
    return it == rev_.end() ? 0 : it->second.owner;
  }
  // Called once an order is no longer live; its client id may then be reused.
  void retire(OrderId x) {
    const auto it = rev_.find(x);
    if (it == rev_.end()) return;
    fwd_.erase(it->second);
    rev_.erase(it);
  }
  std::size_t live() const { return rev_.size(); }
  OrderId next_exchange_id() const { return next_; }

 private:
  struct Hasher {
    std::uint64_t k0, k1;
    std::size_t operator()(const Key& k) const noexcept { return siphash<1, 3>(k0, k1, &k, sizeof k); }
  };
  static std::uint64_t random_key() {
    std::random_device rd;
    return (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
  }
  std::unordered_map<Key, OrderId, Hasher> fwd_;
  std::unordered_map<OrderId, Key> rev_;  // exchange ids are sequential and chosen here: not attackable
  OrderId next_;
};

}  // namespace exsim
