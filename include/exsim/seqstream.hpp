// The sequenced command stream: how the primary replicates its input to hot backups (Linux only).
//
// This is the architecture in Brian Nigito's "How to Build an Exchange" talk, reduced to its core. The
// matching engine is a deterministic state machine, so replicating it means replicating its INPUT: every
// command, in the sequencer's order. The primary publishes that order over UDP multicast; any number of
// replicas apply it and reach bit-identical state (checked with event digests).
//
// UDP loses and reorders datagrams, so the stream carries its own recovery:
//   * Every datagram names the sequence number of its first command. A receiver that sees a jump knows
//     exactly which range it is missing, stashes what arrived early, and asks the primary's retransmitter
//     (a unicast control socket) for the gap. Duplicates are ignored, so a retransmission that races the
//     original is harmless.
//   * When idle, the primary sends heartbeats carrying the next sequence number. Without them a receiver
//     cannot distinguish "no traffic" from "the last datagram was lost" (tail loss).
//   * The retransmitter serves from an in-memory ring of recent commands. A receiver that falls further
//     behind than the ring gets an explicit Unavailable reply rather than silence; it would then need a
//     snapshot, which this code does not implement (see docs/DESIGN.md).
//   * Replicas acknowledge the highest sequence number they have applied AND flushed to their own
//     journal. With --replicate-wait the primary releases a client's acknowledgement only once some
//     replica has acknowledged that command, so a promoted backup never lacks an acknowledged order.
//
// Wire format: a 24-byte Header, then `count` raw 56-byte Commands for Data. Both ends run on one host
// (little endian, same struct layout), as a real deployment's engine replicas would; a cross-platform
// feed would use explicit field encoding like protocol.hpp.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "exsim/common.hpp"
#include "exsim/workload.hpp"

namespace exsim::seqstream {

inline constexpr std::uint32_t kMagic = 0x51535845;  // "EXSQ"

enum class Kind : std::uint8_t {
  Data = 1,         // first_seq = seq of the first command; count commands follow
  Heartbeat = 2,    // first_seq = next seq the primary will assign
  End = 3,          // orderly shutdown; first_seq = next seq (nothing more will be assigned)
  Retransmit = 4,   // replica -> primary: send [first_seq, first_seq + count)
  Ack = 5,          // replica -> primary: everything through first_seq is applied and journaled
  Unavailable = 6,  // primary -> replica: the ring no longer holds the request; first_seq = oldest held
};

struct Header {
  std::uint32_t magic;
  std::uint32_t session;
  std::uint64_t first_seq;
  std::uint16_t count;
  Kind kind;
  std::uint8_t pad[5];
};
static_assert(sizeof(Header) == 24);

inline constexpr std::size_t kMaxDatagram = 1472;  // 1500-byte Ethernet MTU minus IPv4 and UDP headers
inline constexpr std::size_t kPerPacket = (kMaxDatagram - sizeof(Header)) / sizeof(Command);  // 25
static_assert(kPerPacket == 25);

inline std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

// "a.b.c.d:port"
inline sockaddr_in parse_endpoint(const std::string& s) {
  const auto colon = s.rfind(':');
  if (colon == std::string::npos) throw std::invalid_argument("expected ip:port, got " + s);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(std::stoul(s.substr(colon + 1))));
  if (inet_pton(AF_INET, s.substr(0, colon).c_str(), &a.sin_addr) != 1) throw std::invalid_argument("bad address " + s);
  return a;
}

inline int udp_socket(std::uint16_t bind_port, bool reuse) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) throw std::runtime_error("socket failed");
  int one = 1, buf = 16 << 20;
  if (reuse) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
  sockaddr_in a{};
  a.sin_family = AF_INET, a.sin_port = htons(bind_port), a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    throw std::runtime_error("bind failed on udp port " + std::to_string(bind_port));
  }
  return fd;
}

inline Header header(Kind k, std::uint32_t session, std::uint64_t first_seq, std::uint16_t count) {
  Header h{};
  h.magic = kMagic, h.session = session, h.first_seq = first_seq, h.count = count, h.kind = k;
  return h;
}

// ---------------------------------------------------------------------------------------------------------
// Primary side: publishes the sequenced stream, serves retransmissions, collects acknowledgements.
// ---------------------------------------------------------------------------------------------------------
class Publisher {
 public:
  struct Stats {
    std::uint64_t packets = 0, dropped = 0, heartbeats = 0, retransmit_requests = 0, retransmitted_packets = 0,
                  unavailable = 0, acks = 0;
  };

  // `drop` is fault injection: the probability that an original multicast datagram is silently not sent
  // (it stays in the ring, so it can be retransmitted). Deterministic for a given seed.
  Publisher(const std::string& group, std::uint16_t control_port, std::uint32_t ring_log2, double drop,
            std::uint64_t seed, std::uint64_t next_seq)
      : group_(parse_endpoint(group)),
        fd_(udp_socket(control_port, false)),
        ring_(std::size_t{1} << ring_log2),
        mask_((std::uint64_t{1} << ring_log2) - 1),
        drop_(drop),
        rng_(seed),
        next_seq_(next_seq),
        pending_first_(next_seq),
        acked_(next_seq - 1) {
    session_ = static_cast<std::uint32_t>(now_ns() >> 10) | 1u;
    in_addr loop{};
    loop.s_addr = htonl(INADDR_LOOPBACK);
    int on = 1;
    setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &loop, sizeof loop);
    setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &on, sizeof on);
  }
  ~Publisher() { ::close(fd_); }
  Publisher(const Publisher&) = delete;
  Publisher& operator=(const Publisher&) = delete;

  int control_fd() const { return fd_; }
  std::uint64_t acked() const { return acked_; }
  const Stats& stats() const { return stats_; }

  // Stages a command; nothing is sent until flush(). Commands must arrive in sequence order with no holes (the
  // sequencer's job).
  void publish(const Command& c) {
    if (c.seq != next_seq_) throw std::logic_error("Publisher: out-of-sequence command");
    ring_[c.seq & mask_] = c;
    ++next_seq_;
  }

  // Sends everything staged, kPerPacket commands per datagram. The gateway calls this once per batch, AFTER
  // flushing its journal: a replica must never hold a command the primary's own journal could lose, so the
  // backup's journal is always a prefix of the primary's (scripts/e2e_replication.sh checks this after kill -9).
  void flush() {
    while (pending_first_ < next_seq_) {
      const auto n = static_cast<std::uint16_t>(std::min<std::uint64_t>(kPerPacket, next_seq_ - pending_first_));
      if (drop_ > 0 && rng_.chance(drop_))
        ++stats_.dropped;
      else
        send_data(pending_first_, n, group_);
      ++stats_.packets;
      pending_first_ += n;
      last_send_ns_ = now_ns();
    }
  }

  void heartbeat_if_idle(std::uint64_t now, std::uint64_t interval_ns = 20'000'000) {
    if (now - last_send_ns_ < interval_ns) return;
    flush();
    send_control(header(Kind::Heartbeat, session_, next_seq_, 0), group_);
    ++stats_.heartbeats;
    last_send_ns_ = now;
  }

  void end() {
    flush();
    for (int i = 0; i < 3; ++i) send_control(header(Kind::End, session_, next_seq_, 0), group_);
  }

  // Drains the control socket: retransmission requests and acknowledgements. Never blocks.
  void service() {
    alignas(8) std::byte buf[kMaxDatagram];
    for (;;) {
      sockaddr_in from{};
      socklen_t len = sizeof from;
      const ssize_t n = ::recvfrom(fd_, buf, sizeof buf, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), &len);
      if (n < static_cast<ssize_t>(sizeof(Header))) {
        if (n < 0) return;
        continue;
      }
      Header h;
      std::memcpy(&h, buf, sizeof h);
      if (h.magic != kMagic || h.session != session_) continue;
      if (h.kind == Kind::Ack) {
        ++stats_.acks;
        if (h.first_seq > acked_ && h.first_seq < next_seq_) acked_ = h.first_seq;
      } else if (h.kind == Kind::Retransmit) {
        ++stats_.retransmit_requests;
        const std::uint64_t oldest = next_seq_ > ring_.size() ? next_seq_ - ring_.size() : 1;
        if (h.first_seq < oldest) {
          send_control(header(Kind::Unavailable, session_, oldest, 0), from);
          ++stats_.unavailable;
          continue;
        }
        const std::uint64_t end = std::min<std::uint64_t>(h.first_seq + h.count, next_seq_);
        for (std::uint64_t s = h.first_seq; s < end; s += kPerPacket) {
          send_data(s, static_cast<std::uint16_t>(std::min<std::uint64_t>(kPerPacket, end - s)), from);
          ++stats_.retransmitted_packets;
        }
      }
    }
  }

 private:
  void send_data(std::uint64_t first, std::uint16_t n, const sockaddr_in& to) {
    alignas(8) std::byte buf[kMaxDatagram];
    const Header h = header(Kind::Data, session_, first, n);
    std::memcpy(buf, &h, sizeof h);
    for (std::uint16_t i = 0; i < n; ++i)
      std::memcpy(buf + sizeof h + i * sizeof(Command), &ring_[(first + i) & mask_], sizeof(Command));
    ::sendto(fd_, buf, sizeof h + n * sizeof(Command), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
  }
  void send_control(const Header& h, const sockaddr_in& to) {
    ::sendto(fd_, &h, sizeof h, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
  }

  sockaddr_in group_;
  int fd_;
  std::vector<Command> ring_;
  std::uint64_t mask_;
  double drop_;
  Rng rng_;
  std::uint32_t session_ = 0;
  std::uint64_t next_seq_, pending_first_, acked_;
  std::uint64_t last_send_ns_ = 0;
  Stats stats_;
};

// ---------------------------------------------------------------------------------------------------------
// Replica side: delivers every command exactly once, in sequence order, recovering gaps as it goes.
// ---------------------------------------------------------------------------------------------------------
class Subscriber {
 public:
  struct Stats {
    std::uint64_t packets = 0, duplicates = 0, gaps = 0, requests = 0, stashed_max = 0;
  };

  Subscriber(const std::string& group, const std::string& primary_control, std::uint64_t first_seq = 1)
      : primary_(parse_endpoint(primary_control)), expected_(first_seq) {
    const sockaddr_in g = parse_endpoint(group);
    mfd_ = udp_socket(ntohs(g.sin_port), true);
    ip_mreq m{};
    m.imr_multiaddr = g.sin_addr;
    m.imr_interface.s_addr = htonl(INADDR_LOOPBACK);
    if (setsockopt(mfd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) != 0) throw std::runtime_error("join failed");
    cfd_ = udp_socket(0, false);
  }
  ~Subscriber() { ::close(mfd_), ::close(cfd_); }
  Subscriber(const Subscriber&) = delete;
  Subscriber& operator=(const Subscriber&) = delete;

  std::uint64_t expected() const { return expected_; }  // next seq to deliver
  bool ended() const { return ended_ && expected_ >= end_seq_; }
  bool heard() const { return session_ != 0; }
  std::uint64_t last_heard_ns() const { return last_heard_; }
  bool unavailable() const { return unavailable_; }
  const Stats& stats() const { return stats_; }

  // Waits up to timeout_ms for traffic, then calls on_command(const Command&) for every command that is now
  // deliverable in order. Returns the number delivered.
  template <class F>
  std::size_t poll(int timeout_ms, F&& on_command) {
    pollfd p[2] = {{mfd_, POLLIN, 0}, {cfd_, POLLIN, 0}};
    ::poll(p, 2, timeout_ms);
    std::size_t delivered = 0;
    for (const int fd : {mfd_, cfd_}) delivered += drain(fd, on_command);
    request_gap();
    return delivered;
  }

  void ack(std::uint64_t through) {
    if (session_ == 0) return;
    const Header h = header(Kind::Ack, session_, through, 0);
    ::sendto(cfd_, &h, sizeof h, 0, reinterpret_cast<const sockaddr*>(&primary_), sizeof primary_);
  }

 private:
  template <class F>
  std::size_t drain(int fd, F& on_command) {
    alignas(8) std::byte buf[kMaxDatagram];
    std::size_t delivered = 0;
    for (;;) {
      const ssize_t n = ::recv(fd, buf, sizeof buf, MSG_DONTWAIT);
      if (n < 0) return delivered;
      if (n < static_cast<ssize_t>(sizeof(Header))) continue;
      Header h;
      std::memcpy(&h, buf, sizeof h);
      if (h.magic != kMagic) continue;
      if (session_ == 0) session_ = h.session;  // lock on to the first primary heard
      if (h.session != session_) continue;
      last_heard_ = now_ns();
      switch (h.kind) {
        case Kind::Data: {
          if (static_cast<std::size_t>(n) != sizeof h + h.count * sizeof(Command)) continue;
          ++stats_.packets;
          if (h.first_seq + h.count <= expected_) {
            ++stats_.duplicates;
            continue;
          }
          std::vector<Command> cmds(h.count);
          std::memcpy(cmds.data(), buf + sizeof h, h.count * sizeof(Command));
          if (h.first_seq > expected_) {
            if (stash_.emplace(h.first_seq, std::move(cmds)).second && stash_.size() == 1) ++stats_.gaps;
            stats_.stashed_max = std::max<std::uint64_t>(stats_.stashed_max, stash_.size());
            continue;
          }
          delivered += deliver(h.first_seq, cmds, on_command);
          // The gap may now be closed: deliver whatever was stashed behind it.
          while (!stash_.empty() && stash_.begin()->first <= expected_) {
            auto node = stash_.extract(stash_.begin());
            if (node.key() + node.mapped().size() > expected_) delivered += deliver(node.key(), node.mapped(), on_command);
          }
          break;
        }
        case Kind::Heartbeat:
          known_next_ = std::max(known_next_, h.first_seq);
          break;
        case Kind::End:
          ended_ = true, end_seq_ = h.first_seq;
          known_next_ = std::max(known_next_, h.first_seq);
          break;
        case Kind::Unavailable:
          unavailable_ = true;
          break;
        default:
          break;
      }
    }
  }

  template <class F>
  std::size_t deliver(std::uint64_t first, const std::vector<Command>& cmds, F& on_command) {
    std::size_t k = 0;
    for (std::size_t i = expected_ - first; i < cmds.size(); ++i, ++k) on_command(cmds[i]);
    expected_ = first + cmds.size();
    known_next_ = std::max(known_next_, expected_);
    return k;
  }

  // Asks for [expected_, gap end) if something newer is known to exist. Re-asks after 20 ms without progress.
  void request_gap() {
    std::uint64_t gap_end = known_next_;
    if (!stash_.empty()) gap_end = stash_.begin()->first;
    if (gap_end <= expected_ || session_ == 0) return;
    const std::uint64_t now = now_ns();
    if (requested_from_ == expected_ && now - requested_at_ < 20'000'000) return;
    const auto count = static_cast<std::uint16_t>(std::min<std::uint64_t>(gap_end - expected_, 4096));
    const Header h = header(Kind::Retransmit, session_, expected_, count);
    ::sendto(cfd_, &h, sizeof h, 0, reinterpret_cast<const sockaddr*>(&primary_), sizeof primary_);
    ++stats_.requests;
    requested_from_ = expected_, requested_at_ = now;
  }

  sockaddr_in primary_;
  int mfd_ = -1, cfd_ = -1;
  std::uint32_t session_ = 0;
  std::uint64_t expected_, known_next_ = 0, end_seq_ = 0;
  std::uint64_t requested_from_ = 0, requested_at_ = 0, last_heard_ = 0;
  bool ended_ = false, unavailable_ = false;
  std::map<std::uint64_t, std::vector<Command>> stash_;
  Stats stats_;
};

}  // namespace exsim::seqstream
