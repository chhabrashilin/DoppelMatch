// Public market data: a sequenced level-2 feed over UDP multicast, recovered by snapshots (Linux only).
//
// The recovery model is the one real venues use for market data (CME's MDP 3.0 is the best-known example), and it
// differs on purpose from the replication stream in seqstream.hpp. Replicas must never miss a command, so they ask
// for retransmission; market data has thousands of consumers, so nobody is served individually. Instead:
//   * Incremental packets carry, per batch, the new total quantity of every price level that changed (0 removes
//     it) and every trade, under a packet sequence number.
//   * Every --md-snapshot-every incremental packets the full book of every symbol is published as a snapshot,
//     tagged with the last incremental sequence number it reflects, split across as many datagrams as needed.
//   * A subscriber that sees a sequence gap discards its book, buffers what arrives, and rebuilds from the next
//     snapshot, then applies the buffered incrementals newer than it. A late joiner does the same.
//   * Heartbeats carry the last incremental sequence number, so a lost final packet is detected too.
// Updates are published only after a batch is committed (journaled, and replicated when the primary waits for a
// backup), so the feed never shows state that a failover could roll back.
//
// Wire format: a 32-byte MdHeader, then `count` 24-byte MdEntry records. Same-host layout, as in seqstream.hpp.
#pragma once

#include <algorithm>
#include <bit>
#include <iterator>
#include <map>
#include <vector>

#include "exsim/seqstream.hpp"

namespace exsim::md_feed {

inline constexpr std::uint32_t kMagic = 0x464D5845;  // "EXMF"

enum class Kind : std::uint32_t { Incremental = 1, Snapshot = 2, Heartbeat = 3, End = 4 };
enum class EntryType : std::uint8_t { Level = 1, Trade = 2 };

struct MdHeader {
  std::uint32_t magic;
  Kind kind;
  std::uint64_t seq;       // Incremental: this packet's sequence number; Heartbeat/End: the last one sent
  std::uint64_t snap_seq;  // Snapshot: the last incremental sequence number the snapshot reflects
  std::uint16_t count, part, parts, pad;
};
static_assert(sizeof(MdHeader) == 32);

struct MdEntry {
  std::int64_t price;
  std::uint64_t qty;  // Level: the new total at the price (0 = removed); Trade: the traded quantity
  std::uint32_t symbol;
  Side side;          // Level: the book side; Trade: the aggressor's side
  EntryType type;
  std::uint8_t pad[2];
};
static_assert(sizeof(MdEntry) == 24);

inline constexpr std::size_t kPerPacket = (seqstream::kMaxDatagram - sizeof(MdHeader)) / sizeof(MdEntry);  // 60

// A digest of the level-2 book of every symbol (sides, prices and quantities, in order). The server prints it for
// its engine, a subscriber for the book it rebuilt from the feed; they must be equal.
class L2Digest {
 public:
  void add(std::uint32_t symbol, Side side, Price px, Qty qty) {
    const std::uint64_t w[3] = {(std::uint64_t{symbol} << 1) | static_cast<std::uint64_t>(side),
                                static_cast<std::uint64_t>(px), qty};
    for (const std::uint64_t x : w) h_ = std::rotl((h_ ^ x) * 0x9E3779B97F4A7C15ull, 29);
    ++n_;
  }
  std::uint64_t value() const { return h_ ^ n_; }

 private:
  std::uint64_t h_ = 0x243F6A8885A308D3ull, n_ = 0;
};

template <class Engine>
std::uint64_t engine_l2_digest(const Engine& engine) {
  L2Digest d;
  for (std::uint32_t s = 0; s < engine.num_symbols(); ++s)
    for (const Side side : {Side::Buy, Side::Sell})
      engine.book(s).for_each_level(side, [&](Price px, Qty q) { d.add(s, side, px, q); });
  return d.value();
}

// ---------------------------------------------------------------------------------------------------------
// Publisher: owned by the gateway.
// ---------------------------------------------------------------------------------------------------------
class Publisher {
 public:
  struct Stats {
    std::uint64_t packets = 0, dropped = 0, snapshots = 0, levels = 0, trades = 0;
  };

  // `drop` is fault injection on incremental packets (never on snapshots): the subscriber must recover.
  Publisher(const std::string& group, std::uint64_t snapshot_every, double drop, std::uint64_t seed)
      : group_(seqstream::parse_endpoint(group)), fd_(seqstream::udp_socket(0, false)), every_(snapshot_every),
        drop_(drop), rng_(seed) {
    in_addr loop{};
    loop.s_addr = htonl(INADDR_LOOPBACK);
    int on = 1;
    setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &loop, sizeof loop);
    setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &on, sizeof on);
  }
  ~Publisher() { ::close(fd_); }
  Publisher(const Publisher&) = delete;
  Publisher& operator=(const Publisher&) = delete;

  const Stats& stats() const { return stats_; }

  void level(std::uint32_t symbol, Side side, Price px, Qty qty) {
    staged_.push_back(MdEntry{px, qty, symbol, side, EntryType::Level, {}});
    ++stats_.levels;
  }
  void trade(std::uint32_t symbol, Side aggressor, Price px, Qty qty) {
    staged_.push_back(MdEntry{px, qty, symbol, aggressor, EntryType::Trade, {}});
    ++stats_.trades;
  }

  // Sends the staged entries as incremental packets; publishes a snapshot when one is due. The snapshot is taken
  // only at the end of the batch, so the sequence number it carries is the last packet of the state it shows.
  template <class Engine>
  void flush(const Engine& engine) {
    for (std::size_t i = 0; i < staged_.size(); i += kPerPacket) {
      const auto n = static_cast<std::uint16_t>(std::min(kPerPacket, staged_.size() - i));
      ++seq_;
      if (drop_ > 0 && rng_.chance(drop_))
        ++stats_.dropped;
      else
        send(Kind::Incremental, seq_, 0, 0, 1, staged_.data() + i, n);
      ++stats_.packets;
      ++since_snapshot_;
    }
    if (every_ != 0 && since_snapshot_ >= every_) snapshot(engine);
    staged_.clear();
    last_send_ns_ = seqstream::now_ns();
  }

  template <class Engine>
  void snapshot(const Engine& engine) {
    std::vector<MdEntry> all;
    for (std::uint32_t s = 0; s < engine.num_symbols(); ++s)
      for (const Side side : {Side::Buy, Side::Sell})
        engine.book(s).for_each_level(side, [&](Price px, Qty q) { all.push_back(MdEntry{px, q, s, side, EntryType::Level, {}}); });
    const auto parts = static_cast<std::uint16_t>(std::max<std::size_t>(1, (all.size() + kPerPacket - 1) / kPerPacket));
    for (std::uint16_t p = 0; p < parts; ++p) {
      const std::size_t from = std::size_t{p} * kPerPacket;
      const auto n = static_cast<std::uint16_t>(std::min(kPerPacket, all.size() - std::min(all.size(), from)));
      send(Kind::Snapshot, 0, seq_, p, parts, all.data() + from, n);
    }
    ++stats_.snapshots;
    since_snapshot_ = 0;
  }

  void heartbeat_if_idle(std::uint64_t now, std::uint64_t interval_ns = 50'000'000) {
    if (now - last_send_ns_ < interval_ns) return;
    send(Kind::Heartbeat, seq_, 0, 0, 1, nullptr, 0);
    last_send_ns_ = now;
  }

  // Orderly end of the feed: a final snapshot, so every subscriber can end exactly in sync, then End.
  template <class Engine>
  void end(const Engine& engine) {
    snapshot(engine);
    for (int i = 0; i < 3; ++i) send(Kind::End, seq_, seq_, 0, 1, nullptr, 0);
  }

 private:
  void send(Kind k, std::uint64_t seq, std::uint64_t snap_seq, std::uint16_t part, std::uint16_t parts,
            const MdEntry* e, std::uint16_t n) {
    alignas(8) std::byte buf[seqstream::kMaxDatagram];
    const MdHeader h{kMagic, k, seq, snap_seq, n, part, parts, 0};
    std::memcpy(buf, &h, sizeof h);
    if (n) std::memcpy(buf + sizeof h, e, n * sizeof(MdEntry));
    ::sendto(fd_, buf, sizeof h + n * sizeof(MdEntry), 0, reinterpret_cast<const sockaddr*>(&group_), sizeof group_);
  }

  sockaddr_in group_;
  int fd_;
  std::uint64_t every_, since_snapshot_ = 0, seq_ = 0, last_send_ns_ = 0;
  double drop_;
  Rng rng_;
  std::vector<MdEntry> staged_;
  Stats stats_;
};

// ---------------------------------------------------------------------------------------------------------
// Subscriber: rebuilds every symbol's level-2 book from the feed.
// ---------------------------------------------------------------------------------------------------------
class Book {
 public:
  struct Stats {
    std::uint64_t incrementals = 0, gaps = 0, snapshots_used = 0, trades = 0, buffered_max = 0;
  };

  explicit Book(const std::string& group) : fd_(seqstream::group_socket(seqstream::parse_endpoint(group))) {}
  ~Book() { ::close(fd_); }
  Book(const Book&) = delete;
  Book& operator=(const Book&) = delete;

  bool synced() const { return synced_; }
  bool ended() const { return ended_; }
  const Stats& stats() const { return stats_; }
  std::uint64_t last_seq() const { return seq_; }

  // Waits up to timeout_ms, then processes everything that has arrived.
  void poll(int timeout_ms) {
    pollfd p{fd_, POLLIN, 0};
    ::poll(&p, 1, timeout_ms);
    alignas(8) std::byte buf[seqstream::kMaxDatagram];
    for (;;) {
      const ssize_t n = ::recv(fd_, buf, sizeof buf, MSG_DONTWAIT);
      if (n < 0) return;
      if (n < static_cast<ssize_t>(sizeof(MdHeader))) continue;
      MdHeader h;
      std::memcpy(&h, buf, sizeof h);
      if (h.magic != kMagic || static_cast<std::size_t>(n) != sizeof h + h.count * sizeof(MdEntry)) continue;
      std::vector<MdEntry> e(h.count);
      if (h.count) std::memcpy(e.data(), buf + sizeof h, h.count * sizeof(MdEntry));
      on_packet(h, e);
    }
  }

  std::uint64_t digest() const {
    L2Digest d;
    for (const auto& [sym, sides] : books_) {
      for (auto it = sides.bids.rbegin(); it != sides.bids.rend(); ++it) d.add(sym, Side::Buy, it->first, it->second);
      for (const auto& [px, q] : sides.asks) d.add(sym, Side::Sell, px, q);
    }
    return d.value();
  }

 private:
  struct Sides {
    std::map<Price, Qty> bids, asks;
  };

  void on_packet(const MdHeader& h, const std::vector<MdEntry>& e) {
    switch (h.kind) {
      case Kind::Incremental:
        if (synced_ && h.seq <= seq_) return;  // duplicate
        if (synced_ && h.seq == seq_ + 1) {
          apply(e), seq_ = h.seq, ++stats_.incrementals;
          return;
        }
        if (synced_) ++stats_.gaps, synced_ = false, books_.clear();  // a gap: the book is unknown
        pending_[h.seq] = e;  // keep it: a snapshot may reflect less than this
        stats_.buffered_max = std::max<std::uint64_t>(stats_.buffered_max, pending_.size());
        return;
      case Kind::Heartbeat:
      case Kind::End:
        if (synced_ && h.seq > seq_) ++stats_.gaps, synced_ = false, books_.clear();  // lost the last packets
        if (h.kind == Kind::End) end_seq_ = h.seq, end_seen_ = true;
        break;
      case Kind::Snapshot:
        if (synced_) return;  // not needed
        on_snapshot_part(h, e);
        break;
    }
    ended_ = end_seen_ && synced_ && seq_ >= end_seq_;
  }

  void on_snapshot_part(const MdHeader& h, const std::vector<MdEntry>& e) {
    if (h.part == 0 || h.snap_seq != snap_id_) snap_id_ = h.snap_seq, snap_parts_.assign(h.parts, {}), snap_have_ = 0;
    if (h.part >= snap_parts_.size() || !snap_parts_[h.part].empty() || (h.count == 0 && h.parts > 1)) return;
    snap_parts_[h.part] = e.empty() ? std::vector<MdEntry>{MdEntry{}} : e;  // an empty book still counts as received
    if (++snap_have_ < snap_parts_.size()) return;
    books_.clear();  // complete: rebuild from it, then replay the buffered incrementals it does not reflect
    for (const auto& part : snap_parts_) apply(part);
    seq_ = h.snap_seq, synced_ = true, ++stats_.snapshots_used;
    while (!pending_.empty() && pending_.begin()->first <= seq_) pending_.erase(pending_.begin());
    while (!pending_.empty() && pending_.begin()->first == seq_ + 1) {
      apply(pending_.begin()->second), seq_ = pending_.begin()->first, ++stats_.incrementals;
      pending_.erase(pending_.begin());
    }
    if (!pending_.empty()) ++stats_.gaps, synced_ = false, books_.clear();  // still a hole: wait for the next one
    ended_ = end_seen_ && synced_ && seq_ >= end_seq_;
  }

  void apply(const std::vector<MdEntry>& entries) {
    for (const MdEntry& x : entries) {
      if (x.type == EntryType::Trade) {
        ++stats_.trades;
        continue;
      }
      if (x.type != EntryType::Level) continue;  // the placeholder for an empty snapshot part
      auto& side = x.side == Side::Buy ? books_[x.symbol].bids : books_[x.symbol].asks;
      if (x.qty == 0) side.erase(x.price);
      else side[x.price] = x.qty;
    }
    for (auto it = books_.begin(); it != books_.end();) it = it->second.bids.empty() && it->second.asks.empty() ? books_.erase(it) : std::next(it);
  }

  int fd_;
  bool synced_ = false, ended_ = false, end_seen_ = false;
  std::uint64_t seq_ = 0, end_seq_ = 0, snap_id_ = UINT64_MAX, snap_have_ = 0;
  std::map<std::uint32_t, Sides> books_;
  std::map<std::uint64_t, std::vector<MdEntry>> pending_;
  std::vector<std::vector<MdEntry>> snap_parts_;
  Stats stats_;
};

}  // namespace exsim::md_feed
