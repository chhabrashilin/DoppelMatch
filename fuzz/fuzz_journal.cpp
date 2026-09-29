// Fuzzing crash recovery (libFuzzer): the journal reader must cope with any file a crash, a torn write or a bad
// disk can leave behind.
//
// Structure-aware: the first input byte picks N (0-31); N genuinely valid records, derived from the next input
// bytes, are written through JournalWriter, and the rest of the input is appended raw, as a torn, corrupt or
// garbage tail. (Random bytes alone almost never pass a record's CRC, so without the valid prefix the fuzzer
// would hardly get past the first record.) Then:
//   * journal_scan must terminate without a sanitizer report, report consistent counts, never claim more valid
//     bytes than the file holds, and return the N valid records intact, first and in order;
//   * JournalWriter::recover (what exsim_server --recover does) must leave a file that scans Clean and yields
//     exactly the records the damaged file yielded, and appending to it must work.

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "exsim/journal.hpp"

using namespace exsim;

namespace {
[[noreturn]] void fail(const char* what) {
  std::fprintf(stderr, "fuzz_journal: %s\n", what);
  std::abort();
}
bool same(const Command& a, const Command& b) {
  return a.type == b.type && a.order_id == b.order_id && a.seq == b.seq && a.owner == b.owner && a.ts == b.ts &&
         a.symbol == b.symbol && (a.type == MsgType::Cancel || (a.price == b.price && a.qty == b.qty));
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static const std::string path = "/tmp/exsim_fuzz_journal_" + std::to_string(::getpid()) + ".bin";
  std::size_t i = 0;
  auto u8 = [&]() -> std::uint8_t { return i < size ? data[i++] : 0; };
  const unsigned n_valid = size ? u8() % 32 : 0;
  std::vector<Command> written;
  {
    JournalWriter w = JournalWriter::create(path);
    for (unsigned k = 0; k < n_valid; ++k) {
      Command c{};
      const std::uint8_t kind = u8() % 3;
      c.type = kind == 0 ? MsgType::NewOrder : kind == 1 ? MsgType::Cancel : MsgType::Modify;
      c.seq = k + 1, c.order_id = 1 + u8(), c.symbol = u8() % 8, c.owner = 1 + u8(), c.ts = 1000u * (k + 1) + u8();
      if (c.type != MsgType::Cancel) c.price = 1000 + u8(), c.qty = 1 + u8();
      if (c.type == MsgType::NewOrder) c.side = (u8() & 1) ? Side::Sell : Side::Buy;
      w.append(c);
      written.push_back(c);
    }
    w.flush();
  }
  if (i < size) {  // the damaged tail
    std::FILE* f = std::fopen(path.c_str(), "ab");
    if (f == nullptr) return 0;
    std::fwrite(data + i, 1, size - i, f);
    std::fclose(f);
  }
  const auto file_size = static_cast<std::uint64_t>(std::filesystem::file_size(path));

  std::vector<Command> seen;
  const JournalScan s = journal_scan(path, [&](const Command& c) { seen.push_back(c); });
  if (s.records != seen.size()) fail("record count differs from callbacks");
  if (s.valid_bytes > file_size) fail("valid bytes beyond the file");
  if (seen.size() < written.size()) fail("a valid record was lost");
  for (std::size_t k = 0; k < written.size(); ++k)
    if (!same(seen[k], written[k])) fail("a valid record was read back differently");

  {
    JournalWriter w = JournalWriter::recover(path);
    w.flush();
  }
  std::vector<Command> after;
  const JournalScan s2 = journal_scan(path, [&](const Command& c) { after.push_back(c); });
  if (s2.status != JournalStatus::Clean) fail("recovered journal is not clean");
  if (after.size() != seen.size()) fail("recovery changed the number of records");
  for (std::size_t k = 0; k < after.size(); ++k)
    if (!same(after[k], seen[k])) fail("recovery changed a record");
  {
    JournalWriter w = JournalWriter::recover(path);
    Command c{};
    c.type = MsgType::Cancel, c.order_id = 7, c.seq = after.size() + 1;
    w.append(c);
    w.flush();
  }
  std::size_t n = 0;
  const JournalScan s3 = journal_scan(path, [&](const Command&) { ++n; });
  if (s3.status != JournalStatus::Clean || n != after.size() + 1) fail("append after recovery failed");
  return 0;
}
