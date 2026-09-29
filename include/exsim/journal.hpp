// Append-only write-ahead journal of sequenced commands.
//
// Exchanges are replicated state machines: the matching engine is deterministic, so the ordered input
// log IS the state. The sequencer writes each command to the journal BEFORE the engine sees it; after
// a crash, replaying the journal into a fresh engine reproduces the exact pre-crash state (books,
// event stream, risk decisions), and a hot standby can do the same continuously.
//
// File format:  "EXJR" u32 version=1, then records:
//   u32 payload_len | u32 crc32c | u64 ingress_ts | u32 owner | u32 reserved | payload
// The payload is the command in the binary wire format (protocol.hpp). The wire format does not carry
// the ingress timestamp or (for cancels/modifies) the owner, but risk decisions depend on both, so the
// journal records them explicitly. The CRC covers ts, owner and payload.
//
// Failure handling on read:
//   TornTail  the file ends inside a record (power loss / kill -9 mid-write). Expected after a crash;
//             the partial record is discarded.
//   Corrupt   a complete record fails its CRC, or has an impossible length. Reading stops there and
//             everything after it is untrusted, even if it looks valid.
// Either way the valid prefix is returned and `valid_bytes` says where to truncate.
#pragma once

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "exsim/protocol.hpp"

#if defined(__linux__)
#include <unistd.h>
#endif

namespace exsim {

// CRC-32C (Castagnoli), table-driven. Detects all burst errors up to 32 bits and, unlike a plain sum,
// any single flipped bit.
class Crc32c {
 public:
  static std::uint32_t of(const void* data, std::size_t n) noexcept {
    static const Table t = make();
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) c = t.v[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
  }

 private:
  struct Table {
    std::array<std::uint32_t, 256> v{};
  };
  static Table make() {
    Table t;
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
      t.v[i] = c;
    }
    return t;
  }
};

enum class JournalStatus { Clean, TornTail, Corrupt };

struct JournalScan {
  std::uint64_t records = 0;
  std::uint64_t valid_bytes = 0;  // length of the trustworthy prefix, including the file header
  JournalStatus status = JournalStatus::Clean;
};

inline constexpr std::size_t kJournalHeaderBytes = 8;

struct RecordMeta {
  std::uint32_t len;
  std::uint32_t crc;
  std::uint64_t ts;
  std::uint32_t owner;
  std::uint32_t reserved;
};
static_assert(sizeof(RecordMeta) == 24);
inline constexpr std::size_t kCrcPrefix = 12;  // ts + owner, hashed ahead of the payload

// Reads the journal, calling f(const Command&) for every valid record in order.
template <class F>
JournalScan journal_scan(const std::string& path, F&& f) {
  std::FILE* fp = std::fopen(path.c_str(), "rb");
  if (fp == nullptr) throw std::runtime_error("cannot open journal " + path);
  JournalScan s;
  char hdr[kJournalHeaderBytes];
  if (std::fread(hdr, 1, sizeof hdr, fp) != sizeof hdr) {
    std::fclose(fp);
    s.status = JournalStatus::TornTail;  // not even a complete file header
    return s;
  }
  std::uint32_t version;
  std::memcpy(&version, hdr + 4, 4);
  if (std::memcmp(hdr, "EXJR", 4) != 0 || version != 1) {
    std::fclose(fp);
    throw std::runtime_error("not an exsim journal: " + path);
  }
  s.valid_bytes = kJournalHeaderBytes;
  std::vector<std::byte> buf;
  for (;;) {
    RecordMeta m;
    const std::size_t got = std::fread(&m, 1, sizeof m, fp);
    if (got == 0) break;  // clean end of file
    if (got != sizeof m) { s.status = JournalStatus::TornTail; break; }
    if (m.len < sizeof(wire::Header) || m.len > wire::kMaxMessageSize) { s.status = JournalStatus::Corrupt; break; }
    buf.resize(kCrcPrefix + m.len);
    if (std::fread(buf.data() + kCrcPrefix, 1, m.len, fp) != m.len) { s.status = JournalStatus::TornTail; break; }
    std::memcpy(buf.data(), &m.ts, 8);
    std::memcpy(buf.data() + 8, &m.owner, 4);
    if (Crc32c::of(buf.data(), buf.size()) != m.crc) { s.status = JournalStatus::Corrupt; break; }
    Command c;
    const auto r = wire::decode(buf.data() + kCrcPrefix, m.len, c);
    if (r.status != wire::DecodeStatus::Ok || r.consumed != m.len) { s.status = JournalStatus::Corrupt; break; }
    c.ts = m.ts, c.owner = m.owner;
    f(c);
    ++s.records;
    s.valid_bytes += sizeof m + m.len;
  }
  std::fclose(fp);
  return s;
}

class JournalWriter {
 public:
  // Creates a new journal (truncating any existing file).
  static JournalWriter create(const std::string& path) {
    JournalWriter w(path, "wb");
    std::fwrite("EXJR", 1, 4, w.fp_);
    const std::uint32_t v = 1;
    std::fwrite(&v, 1, 4, w.fp_);
    w.sync();
    return w;
  }

  // Opens an existing journal for appending after crash recovery: any torn or corrupt tail is cut off
  // first, so new records always follow a valid prefix. Returns the writer; `scan` reports what was found.
  static JournalWriter recover(const std::string& path, JournalScan* scan = nullptr) {
    const JournalScan s = journal_scan(path, [](const Command&) {});
    if (scan) *scan = s;
    std::filesystem::resize_file(path, s.valid_bytes < kJournalHeaderBytes ? 0 : s.valid_bytes);
    if (s.valid_bytes < kJournalHeaderBytes) return create(path);
    return JournalWriter(path, "ab");
  }

  JournalWriter(JournalWriter&& o) noexcept : fp_(o.fp_), records_(o.records_) { o.fp_ = nullptr; }
  JournalWriter(const JournalWriter&) = delete;
  JournalWriter& operator=(const JournalWriter&) = delete;
  ~JournalWriter() { if (fp_) std::fclose(fp_); }

  // Every write path throws on failure. A journal that silently drops a record (disk full, I/O error) would let the
  // gateway acknowledge a command that recovery cannot bring back; callers must stop acknowledging instead.
  void append(const Command& c) {
    std::byte buf[kCrcPrefix + wire::kMaxMessageSize];
    const std::uint32_t len = static_cast<std::uint32_t>(wire::encode(c, buf + kCrcPrefix));
    std::memcpy(buf, &c.ts, 8);
    std::memcpy(buf + 8, &c.owner, 4);
    const RecordMeta m{len, Crc32c::of(buf, kCrcPrefix + len), c.ts, c.owner, 0};
    if (std::fwrite(&m, 1, sizeof m, fp_) != sizeof m || std::fwrite(buf + kCrcPrefix, 1, len, fp_) != len)
      fail("write");
    ++records_;
  }

  // flush() hands data to the OS (survives a process crash); sync() forces it to stable storage
  // (survives power loss). Buffered writes surface their errors here, so both are checked.
  void flush() {
    if (std::fflush(fp_) != 0) fail("flush");
  }
  void sync() {
    flush();
#if defined(__linux__)
    if (::fsync(::fileno(fp_)) != 0) fail("fsync");
#endif
  }
  std::uint64_t records_written() const { return records_; }

 private:
  JournalWriter(const std::string& path, const char* mode) {
    fp_ = std::fopen(path.c_str(), mode);
    if (fp_ == nullptr) throw std::runtime_error("cannot open journal " + path);
  }
  [[noreturn]] static void fail(const char* what) {
    throw std::runtime_error(std::string("journal ") + what + " failed: " + std::strerror(errno));
  }
  std::FILE* fp_ = nullptr;
  std::uint64_t records_ = 0;
};

}  // namespace exsim
