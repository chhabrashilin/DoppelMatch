// exsim_server: the primary. A TCP order-entry gateway, sequencer and matching engine in one process
// (Linux, epoll). The gateway loop and its ordering guarantees are in gateway.hpp.
//
// With --publish, the sequenced command stream is also multicast to hot backups (exsim_replica), and
// --replicate-wait holds every client acknowledgement until a backup has the command. See seqstream.hpp.
//
//   exsim_server --journal j.bin [--port 9000] [--symbols 8] [--recover] [--sync os|batch|every]
//                [--batch 1000] [--cpu N] [--risk]
//                [--publish 239.255.0.1:31001 --control-port 31002 [--replicate-wait] [--drop 0.01]]
//
// --sync os      flush to the OS after each network batch (survives kill -9, not power loss)   [default]
// --sync batch   also fsync every --batch commands
// --sync every   fsync every command (survives power loss; slow)
// --drop p       fault injection: skip each original multicast datagram with probability p
// --epoch N      this primary's epoch (a promoted backup uses the next one; replicas ignore older epochs)
// --ring-log2 K  retransmission ring of 2^K commands [20]
// --continue-without-replica   with --replicate-wait: keep serving alone if the backup is lost (default: halt)
// --md-publish G       public level-2 market data on multicast group G (mdfeed.hpp), recovered by snapshots
// --md-snapshot-every N  a full snapshot every N incremental packets [500]; --md-drop p: fault injection
// --trust-client-ids   pass client order ids to the engine unchanged, as before exchange-assigned ids
//                      (exists only so scripts/e2e_sessions.py can measure the hash attack it prevents)

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <memory>

#include "args.hpp"
#include "exsim/cpu.hpp"
#include "exsim/journal.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/risk.hpp"
#include "exsim/sinks.hpp"
#include "gateway.hpp"

using namespace exsim;

int main(int argc, char** argv) {
  const tools::Args args(argc, argv);
  if (!args.has("journal")) tools::Args::die("--journal <file> is required");
  const std::string jpath = args.str("journal", "");
  const auto symbols = static_cast<std::uint32_t>(args.u64("symbols", 8));
  tools::GatewayConfig gc;
  gc.port = static_cast<std::uint16_t>(args.u64("port", 9000));
  gc.sync_mode = args.str("sync", "os");
  gc.batch = args.u64("batch", 1000);
  gc.wait_replica = args.has("replicate-wait");
  gc.replica_timeout_ms = args.u64("replica-timeout-ms", 500);
  gc.halt_on_replica_loss = !args.has("continue-without-replica");
  gc.trust_client_ids = args.has("trust-client-ids");
  if (gc.sync_mode != "os" && gc.sync_mode != "batch" && gc.sync_mode != "every") tools::Args::die("--sync must be os, batch or every");
  if (args.has("cpu")) pin_current_thread(static_cast<int>(args.i64("cpu", 0)));
  std::signal(SIGINT, tools::on_stop_signal);
  std::signal(SIGTERM, tools::on_stop_signal);
  std::signal(SIGPIPE, SIG_IGN);

  MatchingEngine<DefaultBook> engine(symbols, BookConfig{});
  RiskConfig rc;
  if (args.has("risk")) {
    rc.max_qty = 100'000, rc.collar_ticks = 5'000, rc.rate_per_sec = 5'000'000, rc.burst = 1'000;
  }
  RiskGate<MatchingEngine<DefaultBook>> gate(engine, rc);
  EventDigest digest;
  std::uint64_t seq = 0;
  OwnerId max_owner = 0;
  OrderId max_order_id = 0;

  // ---- recovery: replay the journal into the fresh engine ----
  std::unique_ptr<JournalWriter> journal;
  if (args.has("recover") && std::filesystem::exists(jpath)) {
    struct Tap { EventDigest& d; void on_event(const Event& e) { d.on_event(e); } } tap{digest};
    const auto t0 = std::chrono::steady_clock::now();
    // replay while scanning (single pass), then reopen for append with any torn tail trimmed
    const JournalScan scan = journal_scan(jpath, [&](const Command& c) {
      gate.process(c, tap);
      seq = c.seq;
      max_owner = std::max(max_owner, c.owner);
      max_order_id = std::max(max_order_id, c.order_id);
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    journal = std::make_unique<JournalWriter>(JournalWriter::recover(jpath));
    std::printf("RECOVERED records=%llu status=%s digest=%016llx last_seq=%llu replay_s=%.3f\n",
                static_cast<unsigned long long>(scan.records),
                scan.status == JournalStatus::Clean ? "clean" : scan.status == JournalStatus::TornTail ? "torn-tail-discarded" : "CORRUPT-tail-discarded",
                static_cast<unsigned long long>(digest.value()), static_cast<unsigned long long>(seq), secs);
  } else {
    journal = std::make_unique<JournalWriter>(JournalWriter::create(jpath));
  }

  std::unique_ptr<seqstream::Publisher> pub;
  if (args.has("publish")) {
    pub = std::make_unique<seqstream::Publisher>(args.str("publish", ""), static_cast<std::uint16_t>(args.u64("control-port", 31002)),
                                                 static_cast<std::uint32_t>(args.u64("ring-log2", 20)), args.f64("drop", 0.0),
                                                 args.u64("seed", 1), seq + 1, static_cast<std::uint32_t>(args.u64("epoch", 1)));
    gc.publisher = pub.get();
  } else if (gc.wait_replica) {
    tools::Args::die("--replicate-wait needs --publish");
  }
  std::unique_ptr<md_feed::Publisher> md;
  if (args.has("md-publish")) {
    md = std::make_unique<md_feed::Publisher>(args.str("md-publish", ""), args.u64("md-snapshot-every", 500),
                                              args.f64("md-drop", 0.0), args.u64("seed", 1) + 1);
    gc.market_data = md.get();
  }
  std::printf("symbols=%u journal=%s\n", symbols, jpath.c_str());
  return tools::run_gateway(gc, engine, gate, *journal, digest, seq, max_owner + 1, max_order_id + 1);
}
