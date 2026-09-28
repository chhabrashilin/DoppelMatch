// exsim_replica: a hot backup of exsim_server (Linux).
//
// Joins the primary's sequenced multicast stream (seqstream.hpp), applies every command, in order and
// exactly once, to its own engine and risk gate, and appends it to its own journal. After each batch it
// flushes the journal and acknowledges the highest sequence number it holds. The primary, run with
// --replicate-wait, releases a client's acknowledgement only after this ack.
//
// Because the engine is deterministic, the replica's state is the primary's state: the event digests
// printed by both sides must be equal.
//
// With --promote-port, the replica is also a standby. When the primary goes silent (no data and no
// heartbeats for --silence-ms), it stops listening, and becomes the primary itself: it opens the gateway on
// --promote-port with its engine, journal and sequence number exactly as they are. Nothing is replayed, so
// failover takes the detection time plus a socket bind. Commands the primary sequenced but never delivered
// here are lost with it; none of them was acknowledged to a client (that is what the wait guarantees).
//
//   exsim_replica --group 239.255.0.1:31001 --primary 127.0.0.1:31002 --journal r.bin
//                 [--symbols 8] [--risk] [--promote-port 9001 --silence-ms 300]

#include <csignal>
#include <cstdio>

#include "args.hpp"
#include "exsim/journal.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/risk.hpp"
#include "exsim/seqstream.hpp"
#include "exsim/sinks.hpp"
#include "gateway.hpp"

using namespace exsim;

int main(int argc, char** argv) {
  const tools::Args args(argc, argv);
  if (!args.has("journal")) tools::Args::die("--journal <file> is required");
  const auto symbols = static_cast<std::uint32_t>(args.u64("symbols", 8));
  const std::uint64_t silence_ns = args.u64("silence-ms", 300) * 1'000'000;
  const bool standby = args.has("promote-port");
  std::signal(SIGINT, tools::on_stop_signal);
  std::signal(SIGTERM, tools::on_stop_signal);
  std::signal(SIGPIPE, SIG_IGN);

  MatchingEngine<DefaultBook> engine(symbols, BookConfig{});
  RiskConfig rc;
  if (args.has("risk")) rc.max_qty = 100'000, rc.collar_ticks = 5'000, rc.rate_per_sec = 5'000'000, rc.burst = 1'000;
  RiskGate<MatchingEngine<DefaultBook>> gate(engine, rc);
  EventDigest digest;
  JournalWriter journal = JournalWriter::create(args.str("journal", ""));
  seqstream::Subscriber sub(args.str("group", "239.255.0.1:31001"), args.str("primary", "127.0.0.1:31002"));

  std::uint64_t seq = 0, last_ack_ns = 0;
  OwnerId max_owner = 0;
  OrderId max_order_id = 0;
  auto apply = [&](const Command& c) {
    journal.append(c);
    gate.process(c, digest);
    seq = c.seq;
    if (c.owner > max_owner) max_owner = c.owner;
    if (c.order_id > max_order_id) max_order_id = c.order_id;
  };
  auto report = [&](const char* what) {
    const auto& s = sub.stats();
    std::printf("%s seq=%llu digest=%016llx packets=%llu duplicates=%llu gaps=%llu retransmit_requests=%llu\n", what,
                static_cast<unsigned long long>(seq), static_cast<unsigned long long>(digest.value()),
                static_cast<unsigned long long>(s.packets), static_cast<unsigned long long>(s.duplicates),
                static_cast<unsigned long long>(s.gaps), static_cast<unsigned long long>(s.requests));
    std::fflush(stdout);
  };
  std::printf("REPLICA_READY\n");
  std::fflush(stdout);

  while (!tools::g_stop) {
    const std::size_t got = sub.poll(1, apply);
    const std::uint64_t now = seqstream::now_ns();
    // Acknowledge only what is in the journal (flushed to the OS). Re-acknowledge periodically: acks are
    // datagrams too, and a lost one must not leave the primary waiting.
    if (got > 0 || now - last_ack_ns > 5'000'000) {
      if (got > 0) journal.flush();
      sub.ack(seq);
      last_ack_ns = now;
    }
    if (sub.unavailable()) {
      report("FATAL_UNAVAILABLE");  // fell behind the retransmission ring; would need a snapshot
      return 1;
    }
    if (sub.ended()) {
      journal.sync();
      report("END");
      return 0;
    }
    if (standby && sub.heard() && now - sub.last_heard_ns() > silence_ns) {
      journal.sync();
      report("PROMOTING");
      tools::GatewayConfig gc;
      gc.port = static_cast<std::uint16_t>(args.u64("promote-port", 9001));
      return tools::run_gateway(gc, engine, gate, journal, digest, seq, max_owner + 1, max_order_id + 1);
    }
  }
  journal.sync();
  report("STOPPED");
  return 0;
}
