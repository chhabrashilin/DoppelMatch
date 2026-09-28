// Binary order-entry wire format and codec.
//
// Fixed-size, little-endian, packed messages with a common 16-byte header, in the spirit of
// NASDAQ OUCH/ITCH. Decoding copies each message with memcpy into a local struct. That is
// alignment-safe and free of strict-aliasing UB, and at these sizes it compiles to a few moves.
// Every field is validated before a Command is produced, and malformed input is reported, never
// trusted.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "exsim/common.hpp"

namespace exsim::wire {

static_assert(std::endian::native == std::endian::little, "wire format is little-endian; add byte swaps for BE hosts");

inline constexpr std::uint8_t kVersion = 1;

#pragma pack(push, 1)
struct Header {
  std::uint16_t length;  // total message length in bytes, header included
  std::uint8_t type;     // MsgType
  std::uint8_t version;
  std::uint32_t symbol;
  std::uint64_t seq;
};
struct NewOrder {
  Header hdr;
  std::uint64_t order_id;
  std::int64_t price;
  std::uint64_t qty;
  std::uint32_t owner;
  std::uint8_t side;
  std::uint8_t ord_type;
  std::uint8_t tif;
  std::uint8_t flags;
};
struct Cancel {
  Header hdr;
  std::uint64_t order_id;
};
struct Modify {
  Header hdr;
  std::uint64_t order_id;
  std::int64_t price;
  std::uint64_t qty;
};
// Outbound execution report: one per Event, plus a Done marker after every command so a client knows
// when the response to its request is complete. hdr.seq echoes the CLIENT's sequence number.
inline constexpr std::uint8_t kReportType = 0x10;
inline constexpr std::uint8_t kReportDone = 0xFF;  // event_type value marking "command finished"
struct ExecReport {
  Header hdr;
  std::uint64_t order_id;
  std::uint64_t maker_id;
  std::int64_t price;
  std::uint64_t qty;
  std::uint64_t leaves;
  std::uint8_t event_type;
  std::uint8_t reason;
  std::uint8_t side;
  std::uint8_t request;
  std::uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 16 && sizeof(NewOrder) == 48 && sizeof(Cancel) == 24 && sizeof(Modify) == 40);
static_assert(sizeof(ExecReport) == 64);
inline constexpr std::size_t kMaxMessageSize = sizeof(NewOrder);
inline constexpr std::size_t kReportSize = sizeof(ExecReport);

inline std::size_t encode_report(const Event& e, std::uint64_t client_seq, std::byte* out) noexcept {
  const ExecReport r{Header{static_cast<std::uint16_t>(sizeof(ExecReport)), kReportType, kVersion, e.symbol, client_seq},
                     e.order_id, e.maker_id, e.price, e.qty, e.leaves, static_cast<std::uint8_t>(e.type),
                     static_cast<std::uint8_t>(e.reason), static_cast<std::uint8_t>(e.side),
                     static_cast<std::uint8_t>(e.request), 0};
  std::memcpy(out, &r, sizeof r);
  return sizeof r;
}
inline std::size_t encode_done(SymbolId symbol, std::uint64_t client_seq, std::byte* out) noexcept {
  const ExecReport r{Header{static_cast<std::uint16_t>(sizeof(ExecReport)), kReportType, kVersion, symbol, client_seq},
                     0, 0, 0, 0, 0, kReportDone, 0, 0, 0, 0};
  std::memcpy(out, &r, sizeof r);
  return sizeof r;
}
// Rebuilds the Event a report was made from. Returns false for Done markers.
inline bool report_to_event(const ExecReport& r, Event& e) noexcept {
  if (r.event_type == kReportDone) return false;
  e = Event{};
  e.order_id = r.order_id, e.maker_id = r.maker_id, e.price = r.price, e.qty = r.qty, e.leaves = r.leaves;
  e.symbol = r.hdr.symbol, e.type = static_cast<EventType>(r.event_type), e.reason = static_cast<Reason>(r.reason);
  e.side = static_cast<Side>(r.side), e.request = static_cast<MsgType>(r.request);
  return true;
}

enum class DecodeStatus : std::uint8_t {
  Ok,
  Incomplete,  // need more bytes; consumed == 0
  Malformed,   // consumed > 0: skip that many bytes and continue. consumed == 0: framing lost, stop.
};

struct DecodeResult {
  DecodeStatus status;
  std::size_t consumed;
};

EXSIM_ALWAYS_INLINE DecodeResult decode(const std::byte* p, std::size_t avail, Command& out) noexcept {
  if (EXSIM_UNLIKELY(avail < sizeof(Header))) return {DecodeStatus::Incomplete, 0};
  Header h;
  std::memcpy(&h, p, sizeof h);
  if (EXSIM_UNLIKELY(h.length < sizeof(Header))) return {DecodeStatus::Malformed, 0};
  if (EXSIM_UNLIKELY(avail < h.length)) return {DecodeStatus::Incomplete, 0};
  const DecodeResult bad{DecodeStatus::Malformed, h.length};
  if (EXSIM_UNLIKELY(h.version != kVersion)) return bad;

  out = Command{};
  out.seq = h.seq;
  out.symbol = h.symbol;
  switch (h.type) {
    case static_cast<std::uint8_t>(MsgType::NewOrder): {
      if (EXSIM_UNLIKELY(h.length != sizeof(NewOrder))) return bad;
      NewOrder m;
      std::memcpy(&m, p, sizeof m);
      if (EXSIM_UNLIKELY(m.side > 1 || m.ord_type > 1 || m.tif > 2)) return bad;
      out.type = MsgType::NewOrder;
      out.order_id = m.order_id;
      out.price = m.price;
      out.qty = m.qty;
      out.owner = m.owner;
      out.side = static_cast<Side>(m.side);
      out.ord_type = static_cast<OrdType>(m.ord_type);
      out.tif = static_cast<Tif>(m.tif);
      out.flags = m.flags;
      return {DecodeStatus::Ok, h.length};
    }
    case static_cast<std::uint8_t>(MsgType::Cancel): {
      if (EXSIM_UNLIKELY(h.length != sizeof(Cancel))) return bad;
      Cancel m;
      std::memcpy(&m, p, sizeof m);
      out.type = MsgType::Cancel;
      out.order_id = m.order_id;
      return {DecodeStatus::Ok, h.length};
    }
    case static_cast<std::uint8_t>(MsgType::Modify): {
      if (EXSIM_UNLIKELY(h.length != sizeof(Modify))) return bad;
      Modify m;
      std::memcpy(&m, p, sizeof m);
      out.type = MsgType::Modify;
      out.order_id = m.order_id;
      out.price = m.price;
      out.qty = m.qty;
      return {DecodeStatus::Ok, h.length};
    }
    default:
      return bad;
  }
}

// Encodes c into out (which must have room for kMaxMessageSize bytes); returns bytes written.
inline std::size_t encode(const Command& c, std::byte* out) noexcept {
  auto header = [&](std::size_t len) {
    return Header{static_cast<std::uint16_t>(len), static_cast<std::uint8_t>(c.type), kVersion, c.symbol, c.seq};
  };
  switch (c.type) {
    case MsgType::NewOrder: {
      const NewOrder m{header(sizeof(NewOrder)), c.order_id, c.price, c.qty, c.owner,
                       static_cast<std::uint8_t>(c.side), static_cast<std::uint8_t>(c.ord_type),
                       static_cast<std::uint8_t>(c.tif), c.flags};
      std::memcpy(out, &m, sizeof m);
      return sizeof m;
    }
    case MsgType::Cancel: {
      const Cancel m{header(sizeof(Cancel)), c.order_id};
      std::memcpy(out, &m, sizeof m);
      return sizeof m;
    }
    case MsgType::Modify: {
      const Modify m{header(sizeof(Modify)), c.order_id, c.price, c.qty};
      std::memcpy(out, &m, sizeof m);
      return sizeof m;
    }
  }
  return 0;
}

}  // namespace exsim::wire
