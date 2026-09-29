// Journal durability/recovery and the pre-trade risk gate.

#include <filesystem>
#include <fstream>

#include "exsim/journal.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/risk.hpp"
#include "exsim/sinks.hpp"
#include "exsim/workload.hpp"
#include "helpers.hpp"

using namespace exsim;
using namespace exsim::test;

namespace {

std::string tmp_path(const char* name) {
  return (std::filesystem::temp_directory_path() / (std::string("exsim_test_") + name)).string();
}

std::vector<Command> some_commands(std::uint64_t n, std::uint32_t symbols = 2) {
  WorkloadConfig wc;
  wc.messages = n;
  wc.symbols = symbols;
  auto v = generate_workload(wc);
  for (auto& c : v) c.ts = c.seq * 1000;  // journaled, like the gateway's ingress stamp
  return v;
}

std::uint64_t digest_of(const std::vector<Command>& cmds, std::size_t upto, std::uint32_t symbols) {
  MatchingEngine<DefaultBook> e(symbols, BookConfig{});
  CountingSink s;
  for (std::size_t i = 0; i < upto; ++i) e.process(cmds[i], s);
  return s.digest();
}

}  // namespace

TEST(crc32c_known_vectors) {
  // RFC 3720 / iSCSI test vectors for CRC-32C.
  const char* nine = "123456789";
  CHECK_EQ(Crc32c::of(nine, 9), 0xE3069283u);
  std::uint8_t zeros[32] = {};
  CHECK_EQ(Crc32c::of(zeros, 32), 0x8A9136AAu);
  std::uint8_t ones[32];
  std::memset(ones, 0xFF, 32);
  CHECK_EQ(Crc32c::of(ones, 32), 0x62A8AB43u);
}

TEST(journal_roundtrip_preserves_every_command) {
  const auto cmds = some_commands(20'000);
  const auto path = tmp_path("rt.jrn");
  {
    auto w = JournalWriter::create(path);
    for (const Command& c : cmds) w.append(c);
    w.sync();
  }
  std::vector<Command> back;
  const auto s = journal_scan(path, [&](const Command& c) { back.push_back(c); });
  CHECK(s.status == JournalStatus::Clean);
  CHECK_EQ(s.records, cmds.size());
  REQUIRE(back.size() == cmds.size());
  // ts and owner ride in the record header (risk decisions depend on them), the rest in the payload
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    if (back[i].type != cmds[i].type || back[i].order_id != cmds[i].order_id || back[i].seq != cmds[i].seq ||
        back[i].symbol != cmds[i].symbol || back[i].ts != cmds[i].ts || back[i].owner != cmds[i].owner) {
      CHECK(false);
      break;
    }
  }
  std::filesystem::remove(path);
}

TEST(journal_torn_tail_is_detected_and_the_valid_prefix_recovers_exactly) {
  const auto cmds = some_commands(5'000);
  const auto path = tmp_path("torn.jrn");
  {
    auto w = JournalWriter::create(path);
    for (const Command& c : cmds) w.append(c);
  }
  const auto full = std::filesystem::file_size(path);
  // simulate kill -9 mid-write at several byte offsets inside the final record
  for (std::uint64_t cut : {1ull, 5ull, 9ull, 20ull}) {
    std::filesystem::resize_file(path, full - cut);
    std::vector<Command> back;
    const auto s = journal_scan(path, [&](const Command& c) { back.push_back(c); });
    CHECK(s.status == JournalStatus::TornTail);
    CHECK_EQ(s.records, cmds.size() - 1);
    // the recovered state equals an engine that only ever saw the first N-1 commands
    MatchingEngine<DefaultBook> e(2, BookConfig{});
    CountingSink sink;
    for (const Command& c : back) e.process(c, sink);
    CHECK_EQ(sink.digest(), digest_of(cmds, cmds.size() - 1, 2));
    // recovery trims the file so appends continue from a clean boundary
    JournalScan found;
    { auto w = JournalWriter::recover(path, &found); w.append(cmds.back()); }
    CHECK(found.status == JournalStatus::TornTail);
    std::uint64_t n = 0;
    const auto s2 = journal_scan(path, [&](const Command&) { ++n; });
    CHECK(s2.status == JournalStatus::Clean);
    CHECK_EQ(n, cmds.size());
    // restore the full file for the next cut
    { auto w = JournalWriter::create(path); for (const Command& c : cmds) w.append(c); }
  }
  std::filesystem::remove(path);
}

TEST(journal_detects_a_flipped_bit_and_distrusts_everything_after_it) {
  const auto cmds = some_commands(2'000);
  const auto path = tmp_path("flip.jrn");
  {
    auto w = JournalWriter::create(path);
    for (const Command& c : cmds) w.append(c);
  }
  // corrupt one byte in the middle of the file
  const auto size = std::filesystem::file_size(path);
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(size / 2));
    char b;
    f.seekg(static_cast<std::streamoff>(size / 2));
    f.read(&b, 1);
    b = static_cast<char>(b ^ 0x10);
    f.seekp(static_cast<std::streamoff>(size / 2));
    f.write(&b, 1);
  }
  std::uint64_t n = 0;
  const auto s = journal_scan(path, [&](const Command&) { ++n; });
  CHECK(s.status == JournalStatus::Corrupt);
  CHECK(n > 0 && n < cmds.size());  // stopped at the bad record, did not skip past it
  CHECK_EQ(s.records, n);
  // and a journal that is not one is refused, not misread
  const auto junk = tmp_path("junk.jrn");
  { std::ofstream j(junk, std::ios::binary); j << "this is not a journal at all"; }
  bool threw = false;
  try { journal_scan(junk, [](const Command&) {}); } catch (const std::runtime_error&) { threw = true; }
  CHECK(threw);
  std::filesystem::remove(path);
  std::filesystem::remove(junk);
}

TEST(journal_write_errors_are_reported_not_swallowed) {
#if defined(__linux__)
  // /dev/full accepts the open and fails every write with ENOSPC. A journal that ignored it would let the gateway
  // acknowledge commands that were never logged.
  if (!std::filesystem::exists("/dev/full")) return;
  bool threw = false;
  try {
    auto w = JournalWriter::create("/dev/full");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
#endif
}

// ---------------- risk ----------------

namespace {
struct RiskFixture {
  MatchingEngine<DefaultBook> engine{1, small_config()};
  VectorSink sink;
  RiskConfig cfg;
  std::unique_ptr<RiskGate<MatchingEngine<DefaultBook>>> gate;
  void make() { gate = std::make_unique<RiskGate<MatchingEngine<DefaultBook>>>(engine, cfg); }
  std::vector<Event> send(const Command& c) {
    sink.events.clear();
    gate->process(c, sink);
    return sink.events;
  }
};
}  // namespace

TEST(risk_rejects_oversized_orders_and_notional) {
  RiskFixture f;
  f.cfg.max_qty = 100;
  f.cfg.max_notional = 100 * 1500;
  f.make();
  CHECK_REJECTED(f.send(new_order(1, Side::Buy, 1500, 101))[0], 1, Reason::RiskMaxQty);
  CHECK_EQ(f.send(new_order(2, Side::Buy, 1500, 100))[0].type, EventType::Accepted);
  CHECK_REJECTED(f.send(new_order(3, Side::Buy, 1501, 100))[0], 3, Reason::RiskMaxNotional);
  CHECK_REJECTED(f.send(modify(2, 1500, 500))[0], 2, Reason::RiskMaxQty);
  CHECK_EQ(f.engine.book(0).order_count(), 1u);  // rejected orders never reached the book
  CHECK_EQ(f.gate->rejected(), 3ull);
}

TEST(risk_notional_check_does_not_overflow) {
  // 2^33 ticks * 2^31 lots wraps a 64-bit product to 0, which would pass any cap if multiplied directly.
  RiskFixture f;
  f.cfg.max_qty = Qty{1} << 40;
  f.cfg.max_notional = 1'000'000;
  f.make();
  Command c = new_order(1, Side::Buy, Price{1} << 33, Qty{1} << 31);
  CHECK_REJECTED(f.send(c)[0], 1, Reason::RiskMaxNotional);
}

TEST(risk_price_collar_tracks_the_last_trade) {
  RiskFixture f;
  f.cfg.collar_ticks = 20;
  f.make();
  // no reference price yet: nothing to collar against
  CHECK_EQ(f.send(new_order(1, Side::Sell, 1500, 10))[0].type, EventType::Accepted);
  f.send(new_order(2, Side::Buy, 1500, 10));  // trades at 1500: reference = 1500
  CHECK_REJECTED(f.send(new_order(3, Side::Buy, 1521, 1))[0], 3, Reason::RiskCollar);
  CHECK_REJECTED(f.send(new_order(4, Side::Sell, 1479, 1))[0], 4, Reason::RiskCollar);
  CHECK_EQ(f.send(new_order(5, Side::Buy, 1520, 1))[0].type, EventType::Accepted);  // exactly at the collar
  CHECK_EQ(f.send(market(6, Side::Buy, 1))[0].type, EventType::Accepted);           // market orders are exempt
}

TEST(risk_rate_limit_allows_a_burst_then_throttles_and_recovers) {
  RiskFixture f;
  f.cfg.rate_per_sec = 1000;  // one message per ms
  f.cfg.burst = 5;
  f.cfg.ticks_per_sec = 1'000'000;  // ts in microseconds
  f.make();
  int accepted = 0, limited = 0;
  for (OrderId id = 1; id <= 20; ++id) {  // 20 messages at the same instant
    Command c = new_order(id, Side::Buy, 1000 + static_cast<Price>(id), 1);
    c.ts = 1'000'000;
    const auto ev = f.send(c);
    (ev[0].type == EventType::Accepted ? accepted : limited)++;
    if (ev[0].type == EventType::Rejected) CHECK(ev[0].reason == Reason::RiskRateLimit);
  }
  CHECK_EQ(accepted, 5);
  CHECK_EQ(limited, 15);
  // cancels are never rate limited: a participant can always reduce risk
  Command k = cancel(1);
  k.ts = 1'000'000;
  CHECK_EQ(f.send(k)[0].type, EventType::Canceled);
  // 10 ms later the bucket has refilled
  Command later = new_order(100, Side::Buy, 1100, 1);
  later.ts = 1'010'000;
  CHECK_EQ(f.send(later)[0].type, EventType::Accepted);
  // an independent owner has an independent budget
  Command other = new_order(101, Side::Buy, 1101, 1, Tif::Day, OrdType::Limit, 0, /*owner*/ 2);
  other.ts = 1'000'000;
  CHECK_EQ(f.send(other)[0].type, EventType::Accepted);
}

TEST(risk_halt_blocks_new_orders_but_not_cancels) {
  RiskFixture f;
  f.make();
  f.send(new_order(1, Side::Buy, 1500, 10));
  f.gate->halt(0);
  CHECK_REJECTED(f.send(new_order(2, Side::Buy, 1500, 10))[0], 2, Reason::RiskHalted);
  CHECK_REJECTED(f.send(modify(1, 1500, 5))[0], 1, Reason::RiskHalted);
  CHECK_EQ(f.send(cancel(1))[0].type, EventType::Canceled);
  f.gate->resume(0);
  CHECK_EQ(f.send(new_order(3, Side::Buy, 1500, 10))[0].type, EventType::Accepted);
  f.gate->halt_all(true);
  CHECK_REJECTED(f.send(new_order(4, Side::Buy, 1500, 10))[0], 4, Reason::RiskHalted);
}

TEST(risk_decisions_are_reproduced_exactly_by_journal_replay) {
  // Risk state depends only on the journaled commands (including ts), so a replay after a crash makes
  // every accept/reject decision the same way.
  auto cmds = some_commands(30'000, 1);
  for (auto& c : cmds) c.owner = 1 + (c.owner % 3);
  RiskConfig cfg;
  cfg.rate_per_sec = 200'000, cfg.burst = 20, cfg.collar_ticks = 120, cfg.max_qty = 2500;
  auto run = [&](const std::vector<Command>& v) {
    MatchingEngine<DefaultBook> e(1, BookConfig{});
    RiskGate<MatchingEngine<DefaultBook>> g(e, cfg);
    CountingSink s;
    for (const Command& c : v) g.process(c, s);
    return std::pair{s.digest(), g.rejected()};
  };
  const auto direct = run(cmds);
  CHECK(direct.second > 100);  // the limits actually bind
  const auto path = tmp_path("risk.jrn");
  { auto w = JournalWriter::create(path); for (const Command& c : cmds) w.append(c); }
  std::vector<Command> replayed;
  journal_scan(path, [&](const Command& c) { replayed.push_back(c); });
  const auto again = run(replayed);
  CHECK_EQ(again.first, direct.first);
  CHECK_EQ(again.second, direct.second);
  std::filesystem::remove(path);
}
