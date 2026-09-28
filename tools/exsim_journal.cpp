// exsim_journal: inspect and replay a journal file offline.
//
//   exsim_journal verify  j.bin      CRC-check every record; report clean / torn tail / corrupt
//   exsim_journal replay  j.bin      replay into a fresh engine; print event digest and book state
//   exsim_journal prefix  a.bin b.bin  check that a holds exactly the first records of b (failover check)
//
// `replay` uses the same engine and risk configuration as exsim_server (defaults), so its digest can be
// compared with the digest the server prints after `--recover`.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "exsim/journal.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/risk.hpp"
#include "exsim/sinks.hpp"

using namespace exsim;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: exsim_journal verify|replay|prefix <journal> [<journal>] [--symbols N] [--risk]\n");
    return 2;
  }
  const std::string cmd = argv[1], path = argv[2];
  std::uint32_t symbols = 8;
  bool risk = false;
  for (int i = 3; i < argc; ++i) {
    if (std::string(argv[i]) == "--symbols" && i + 1 < argc) symbols = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    if (std::string(argv[i]) == "--risk") risk = true;
  }
  try {
    if (cmd == "verify") {
      const auto s = journal_scan(path, [](const Command&) {});
      std::printf("records=%llu valid_bytes=%llu status=%s\n", static_cast<unsigned long long>(s.records),
                  static_cast<unsigned long long>(s.valid_bytes),
                  s.status == JournalStatus::Clean ? "clean" : s.status == JournalStatus::TornTail ? "torn-tail" : "CORRUPT");
      return s.status == JournalStatus::Corrupt ? 1 : 0;
    }
    if (cmd == "prefix") {
      // After a failover: the promoted backup's journal must be a prefix of the dead primary's journal
      // (the primary may hold a few sequenced but unreplicated, hence unacknowledged, commands).
      if (argc < 4) throw std::invalid_argument("usage: exsim_journal prefix <shorter> <longer>");
      std::vector<Command> a;
      journal_scan(path, [&](const Command& c) { a.push_back(c); });
      std::uint64_t i = 0, mismatch = 0;
      const auto s = journal_scan(argv[3], [&](const Command& c) {
        if (i < a.size() && std::memcmp(&a[i], &c, sizeof c) != 0 && mismatch == 0) mismatch = i + 1;
        ++i;
      });
      const bool ok = mismatch == 0 && a.size() <= s.records;
      std::printf("prefix=%s first_records=%llu second_records=%llu%s\n", ok ? "yes" : "NO",
                  static_cast<unsigned long long>(a.size()), static_cast<unsigned long long>(s.records),
                  mismatch ? (" first_mismatch_at_record=" + std::to_string(mismatch)).c_str() : "");
      return ok ? 0 : 1;
    }
    if (cmd == "replay") {
      MatchingEngine<DefaultBook> engine(symbols, BookConfig{});
      RiskConfig rc;
      if (risk) rc.max_qty = 100'000, rc.collar_ticks = 5'000, rc.rate_per_sec = 5'000'000, rc.burst = 1'000;
      RiskGate<MatchingEngine<DefaultBook>> gate(engine, rc);
      CountingSink sink;
      std::uint64_t last_seq = 0;
      const auto s = journal_scan(path, [&](const Command& c) { gate.process(c, sink); last_seq = c.seq; });
      std::uint64_t resting = 0;
      for (std::uint32_t i = 0; i < symbols; ++i) resting += engine.book(i).order_count();
      std::printf("records=%llu status=%s last_seq=%llu events=%llu trades=%llu resting_orders=%llu digest=%016llx\n",
                  static_cast<unsigned long long>(s.records),
                  s.status == JournalStatus::Clean ? "clean" : s.status == JournalStatus::TornTail ? "torn-tail" : "CORRUPT",
                  static_cast<unsigned long long>(last_seq), static_cast<unsigned long long>(sink.events()),
                  static_cast<unsigned long long>(sink.count(EventType::Trade)), static_cast<unsigned long long>(resting),
                  static_cast<unsigned long long>(sink.digest()));
      return 0;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
  return 2;
}
