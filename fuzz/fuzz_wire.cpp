// Fuzzing the order-entry decoder (libFuzzer): the first code that touches bytes from a client.
//
// Properties checked on every input, beyond "no crash, no sanitizer report":
//   * decode never claims more bytes than it was given, and Incomplete never consumes any;
//   * a message that decodes Ok re-encodes to bytes that decode to the same command (a round trip), so what the
//     gateway journals and what it received cannot drift apart.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "exsim/protocol.hpp"

using namespace exsim;

namespace {
[[noreturn]] void fail(const char* what) {
  std::fprintf(stderr, "fuzz_wire: %s\n", what);
  std::abort();
}
bool same_on_wire(const Command& a, const Command& b) {
  if (a.type != b.type || a.order_id != b.order_id || a.symbol != b.symbol || a.seq != b.seq) return false;
  if (a.type == MsgType::NewOrder)
    return a.price == b.price && a.qty == b.qty && a.owner == b.owner && a.side == b.side && a.ord_type == b.ord_type &&
           a.tif == b.tif && a.flags == b.flags;
  if (a.type == MsgType::Modify) return a.price == b.price && a.qty == b.qty;
  return true;
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto* p = reinterpret_cast<const std::byte*>(data);
  std::size_t off = 0;
  while (off < size) {
    Command c;
    const auto r = wire::decode(p + off, size - off, c);
    if (r.consumed > size - off) fail("consumed more than available");
    if (r.status == wire::DecodeStatus::Incomplete) {
      if (r.consumed != 0) fail("Incomplete consumed bytes");
      break;
    }
    if (r.status == wire::DecodeStatus::Malformed && r.consumed == 0) break;  // framing lost: the gateway disconnects
    if (r.status == wire::DecodeStatus::Ok) {
      std::byte buf[wire::kMaxMessageSize];
      const std::size_t n = wire::encode(c, buf);
      Command back;
      const auto r2 = wire::decode(buf, n, back);
      if (r2.status != wire::DecodeStatus::Ok || r2.consumed != n || !same_on_wire(c, back)) fail("round trip changed the command");
    }
    off += r.consumed;
  }
  return 0;
}
