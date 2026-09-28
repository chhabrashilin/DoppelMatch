// The TCP order-entry gateway loop (Linux, epoll), shared by exsim_server and by a promoted exsim_replica.
//
//   client --(binary orders)--> [decode] -> [stamp owner/ts/seq] -> [JOURNAL] -> [risk] -> [engine]
//                                                                      |
//                                                   [PUBLISH to replicas, optionally WAIT for an ack]
//   client <--(exec reports)------------------------------------------------------------ [events]
//
// Ordering guarantees (the point of the design):
//   * Write-ahead: a command is appended to the journal BEFORE the engine sees it, and the journal is
//     flushed (per the sync policy) BEFORE any response for it leaves the process. So a client never
//     holds an acknowledgement for a command that a crash could erase.
//   * Replicated (with a publisher and wait_replica): responses also wait until a replica has
//     acknowledged every command of the batch, so a promoted backup holds every acknowledged command.
//   * Determinism: the engine and risk gate are pure functions of the journaled command stream, so
//     replaying the journal (or the replicated stream) rebuilds the exact state.
//
// The connection id is the participant (owner) id; clients cannot choose it. Nor do they choose the engine's
// order ids: the gateway assigns exchange ids and translates reports back to client ids (client_ids.hpp).
// Every party to a trade gets a report: the taker in response to its order, the maker unsolicited (seq 0).
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "exsim/client_ids.hpp"
#include "exsim/journal.hpp"
#include "exsim/protocol.hpp"
#include "exsim/seqstream.hpp"
#include "exsim/sinks.hpp"

namespace exsim::tools {

inline std::atomic<bool> g_stop{false};
inline void on_stop_signal(int) { g_stop = true; }

struct GatewayConfig {
  std::uint16_t port = 9000;
  std::string sync_mode = "os";  // os | batch | every
  std::uint64_t batch = 1000;
  seqstream::Publisher* publisher = nullptr;
  bool wait_replica = false;
  std::uint64_t replica_timeout_ms = 500;  // then the replica is declared lost and the primary runs alone
  bool trust_client_ids = false;  // pass client ids to the engine unchanged (only to demonstrate the attack)
};

namespace detail {

struct Conn {
  int fd = -1;
  OwnerId owner = 0;
  std::vector<std::byte> in;
  std::size_t in_len = 0;
  std::vector<std::byte> out;
  std::size_t out_off = 0;
  bool want_out = false, paused = false;
};

inline void append_report(std::vector<std::byte>& out, const Event& e, std::uint64_t client_seq) {
  const std::size_t at = out.size();
  out.resize(at + wire::kReportSize);
  wire::encode_report(e, client_seq, out.data() + at);
}

// Receives the engine's events (exchange ids), digests them as they are, and writes each party's view.
struct ReportSink {
  Conn& conn;
  std::uint64_t client_seq;
  OrderId client_id, exchange_id;  // the command's order, in both namespaces
  EventDigest& digest;
  const ClientIdMap& ids;
  std::unordered_map<OwnerId, int>& owner_fd;
  std::unordered_map<int, Conn>& conns;
  std::vector<OrderId>& mentioned;
  std::vector<int>& touched;
  bool translate;
  void on_event(const Event& e) {
    digest.on_event(e);
    if (!translate) return append_report(conn.out, e, client_seq);
    Event r = e;
    r.order_id = e.order_id == exchange_id ? client_id : ids.to_client(e.order_id, conn.owner);
    if (e.maker_id != 0) r.maker_id = ids.to_client(e.maker_id, conn.owner);
    append_report(conn.out, r, client_seq);
    if (e.order_id != exchange_id) mentioned.push_back(e.order_id);
    if (e.maker_id == 0) return;
    mentioned.push_back(e.maker_id);
    const OwnerId maker_owner = ids.owner_of(e.maker_id);
    if (e.type != EventType::Trade || maker_owner == conn.owner) return;
    const auto f = owner_fd.find(maker_owner);  // the maker's session, if still connected
    if (f == owner_fd.end()) return;
    Event m = e;
    m.order_id = 0, m.maker_id = ids.to_client(e.maker_id, maker_owner);  // the maker sees only its own id
    append_report(conns.at(f->second).out, m, 0);
    touched.push_back(f->second);
  }
};

inline void set_nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

// Process CPU time (user + system). Unlike wall-clock, it does not count time spent descheduled.
inline double cpu_seconds() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
         static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
}

}  // namespace detail

// Runs until g_stop. `seq` is the last sequence number assigned (0 for a fresh journal); `next_owner` the
// first owner id to hand out (above every owner already in the stream, after a recovery or promotion).
// `first_exchange_id` must exceed every order id already in the engine.
template <class Engine, class Gate>
int run_gateway(const GatewayConfig& cfg, Engine& engine, Gate& gate, JournalWriter& journal, EventDigest& digest,
                std::uint64_t& seq, OwnerId next_owner, OrderId first_exchange_id) {
  using detail::Conn;
  const int lfd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET, addr.sin_port = htons(cfg.port), addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(lfd, 64) != 0) {
    std::perror("bind/listen");
    return 1;
  }
  detail::set_nonblocking(lfd);
  const int ep = epoll_create1(0);
  epoll_event ev{};
  ev.events = EPOLLIN, ev.data.fd = lfd;
  epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);
  seqstream::Publisher* pub = cfg.publisher;
  if (pub != nullptr) {
    ev.data.fd = pub->control_fd();
    epoll_ctl(ep, EPOLL_CTL_ADD, pub->control_fd(), &ev);
  }
  std::printf("READY port=%u sync=%s replicate=%s\n", cfg.port, cfg.sync_mode.c_str(),
              pub == nullptr ? "off" : cfg.wait_replica ? "wait" : "async");
  std::fflush(stdout);

  std::unordered_map<int, Conn> conns;
  std::unordered_map<OwnerId, int> owner_fd;
  ClientIdMap ids(first_exchange_id);
  std::vector<OrderId> mentioned;
  std::uint64_t handled = 0, since_sync = 0, malformed = 0, waits = 0, wait_ns = 0;
  bool replica_lost = false;
  std::vector<std::byte> chunk(1 << 16);

  auto close_conn = [&](int fd) {
    epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    const auto it = conns.find(fd);
    if (it != conns.end()) owner_fd.erase(it->second.owner), conns.erase(it);
  };
  auto rearm = [&](Conn& c) {
    epoll_event e{};
    e.events = (c.paused ? 0u : static_cast<unsigned>(EPOLLIN)) | (c.want_out ? static_cast<unsigned>(EPOLLOUT) : 0u);
    e.data.fd = c.fd;
    epoll_ctl(ep, EPOLL_CTL_MOD, c.fd, &e);
  };
  auto try_write = [&](Conn& c) {
    while (c.out_off < c.out.size()) {
      const ssize_t n = send(c.fd, c.out.data() + c.out_off, c.out.size() - c.out_off, MSG_NOSIGNAL);
      if (n > 0) { c.out_off += static_cast<std::size_t>(n); continue; }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      return false;  // connection error
    }
    if (c.out_off == c.out.size()) c.out.clear(), c.out_off = 0;
    const bool backlog = c.out.size() - c.out_off > (16u << 20);
    if (c.want_out != (c.out_off < c.out.size()) || c.paused != backlog) {
      c.want_out = c.out_off < c.out.size(), c.paused = backlog;
      rearm(c);
    }
    return true;
  };
  // Blocks until a replica has acknowledged `through`, serving retransmissions and heartbeats meanwhile.
  auto wait_for_replica = [&](std::uint64_t through) {
    const std::uint64_t t0 = seqstream::now_ns();
    ++waits;
    while (pub->acked() < through) {
      pub->service();
      if (pub->acked() >= through) break;
      const std::uint64_t now = seqstream::now_ns();
      if (now - t0 > cfg.replica_timeout_ms * 1'000'000) {
        replica_lost = true;
        std::printf("REPLICA_LOST seq=%llu acked=%llu (continuing without replication)\n",
                    static_cast<unsigned long long>(through), static_cast<unsigned long long>(pub->acked()));
        std::fflush(stdout);
        break;
      }
      pub->heartbeat_if_idle(now, 2'000'000);  // tells a replica about a lost tail datagram quickly
      pollfd p{pub->control_fd(), POLLIN, 0};
      ::poll(&p, 1, 1);
    }
    wait_ns += seqstream::now_ns() - t0;
  };

  // CPU time of the serving loop only: startup (prefaulting the order slabs) can cost seconds on its own.
  const double cpu_start = detail::cpu_seconds();
  epoll_event events[64];
  while (!g_stop) {
    const int n = epoll_wait(ep, events, 64, pub != nullptr ? 10 : 200);
    bool journaled_any = false;
    std::vector<int> touched;
    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;
      if (fd == lfd) {
        for (;;) {
          const int cfd = accept(lfd, nullptr, nullptr);
          if (cfd < 0) break;
          detail::set_nonblocking(cfd);
          setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
          Conn c;
          c.fd = cfd, c.owner = next_owner++;
          owner_fd[c.owner] = cfd;
          conns.emplace(cfd, std::move(c));
          epoll_event ce{};
          ce.events = EPOLLIN, ce.data.fd = cfd;
          epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &ce);
        }
        continue;
      }
      if (pub != nullptr && fd == pub->control_fd()) {
        pub->service();
        continue;
      }
      auto it = conns.find(fd);
      if (it == conns.end()) continue;
      Conn& c = it->second;
      if (events[i].events & EPOLLOUT) {
        if (!try_write(c)) { close_conn(fd); continue; }
      }
      if (!(events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR))) continue;
      const ssize_t got = recv(fd, chunk.data(), chunk.size(), 0);
      if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { close_conn(fd); continue; }
      if (got < 0) continue;
      c.in.resize(c.in_len + static_cast<std::size_t>(got));
      std::memcpy(c.in.data() + c.in_len, chunk.data(), static_cast<std::size_t>(got));
      c.in_len += static_cast<std::size_t>(got);

      std::size_t off = 0;
      bool broken = false;
      while (off < c.in_len) {
        Command cmd;
        const auto r = wire::decode(c.in.data() + off, c.in_len - off, cmd);
        if (r.status == wire::DecodeStatus::Incomplete) break;
        if (r.status == wire::DecodeStatus::Malformed) {
          ++malformed;
          if (r.consumed == 0) { broken = true; break; }  // framing lost: drop the connection
          off += r.consumed;
          continue;
        }
        off += r.consumed;
        const std::uint64_t client_seq = cmd.seq;
        const OrderId client_id = cmd.order_id;
        const ClientIdMap::Key key{client_id, cmd.symbol, c.owner};
        if (!cfg.trust_client_ids) cmd.order_id = cmd.type == MsgType::NewOrder ? ids.for_new(key) : ids.find(key);
        cmd.seq = ++seq;              // the sequencer's order is THE order
        cmd.owner = c.owner;          // identity comes from the session, never from the client
        cmd.ts = seqstream::now_ns();
        journal.append(cmd);          // write-ahead
        if (pub != nullptr) pub->publish(cmd);
        journaled_any = true;
        mentioned.clear();
        detail::ReportSink sink{c, client_seq, client_id, cmd.order_id, digest, ids, owner_fd, conns, mentioned, touched, !cfg.trust_client_ids};
        gate.process(cmd, sink);
        // Orders that are no longer live release their client ids.
        mentioned.push_back(cmd.order_id);
        if (!cfg.trust_client_ids)
          for (const OrderId x : mentioned)
            if (x != 0 && (cmd.symbol >= engine.num_symbols() || !engine.book(cmd.symbol).contains(x))) ids.retire(x);
        const std::size_t at = c.out.size();
        c.out.resize(at + wire::kReportSize);
        wire::encode_done(cmd.symbol, client_seq, c.out.data() + at);
        ++handled;
        if (cfg.sync_mode == "every") journal.sync();
        else if (cfg.sync_mode == "batch" && ++since_sync >= cfg.batch) journal.sync(), since_sync = 0;
      }
      if (broken) { close_conn(fd); continue; }
      c.in_len -= off;
      std::memmove(c.in.data(), c.in.data() + off, c.in_len);
      touched.push_back(fd);
    }
    // Flush the journal to the OS, and hand the batch to the replicas, BEFORE any response leaves.
    if (journaled_any) {
      journal.flush();
      if (pub != nullptr) {
        pub->flush();
        if (cfg.wait_replica && !replica_lost) wait_for_replica(seq);
      }
    }
    if (pub != nullptr) pub->heartbeat_if_idle(seqstream::now_ns());
    for (int fd : touched) {
      auto it = conns.find(fd);
      if (it != conns.end() && !try_write(it->second)) close_conn(fd);
    }
  }

  journal.sync();
  if (pub != nullptr) {
    pub->end();
    const auto& s = pub->stats();
    std::printf("PUBLISHED packets=%llu fault_dropped=%llu heartbeats=%llu retransmit_requests=%llu "
                "retransmitted_packets=%llu unavailable=%llu replica_waits=%llu mean_wait_us=%.1f\n",
                static_cast<unsigned long long>(s.packets), static_cast<unsigned long long>(s.dropped),
                static_cast<unsigned long long>(s.heartbeats), static_cast<unsigned long long>(s.retransmit_requests),
                static_cast<unsigned long long>(s.retransmitted_packets), static_cast<unsigned long long>(s.unavailable),
                static_cast<unsigned long long>(waits), waits ? static_cast<double>(wait_ns) / static_cast<double>(waits) / 1e3 : 0.0);
  }
  const double cpu_s = detail::cpu_seconds() - cpu_start;
  std::printf("STOPPED handled=%llu malformed=%llu risk_rejects=%llu cpu_s=%.3f digest=%016llx seq=%llu\n",
              static_cast<unsigned long long>(handled), static_cast<unsigned long long>(malformed),
              static_cast<unsigned long long>(gate.rejected()), cpu_s,
              static_cast<unsigned long long>(digest.value()), static_cast<unsigned long long>(seq));
  std::fflush(stdout);
  close(lfd);
  close(ep);
  return 0;
}

}  // namespace exsim::tools
