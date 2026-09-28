// Core domain types shared by every component.
//
// Prices are signed integer ticks and quantities unsigned integer lots. There is no floating point
// anywhere on the matching path: decimal scaling is a gateway concern.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#if defined(__GNUC__) || defined(__clang__)
#define EXSIM_LIKELY(x) __builtin_expect(!!(x), 1)
#define EXSIM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define EXSIM_ALWAYS_INLINE inline __attribute__((always_inline))
#define EXSIM_NOINLINE __attribute__((noinline))
#else
#define EXSIM_LIKELY(x) (x)
#define EXSIM_UNLIKELY(x) (x)
#define EXSIM_ALWAYS_INLINE inline
#define EXSIM_NOINLINE
#endif

// ThreadSanitizer (Clang's runtime in particular) links its own global operator new/delete, so the
// allocation-counting replacements in the tests and benchmark are compiled out under TSan.
#if defined(__SANITIZE_THREAD__)
#define EXSIM_TSAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define EXSIM_TSAN_BUILD 1
#endif
#endif
#ifndef EXSIM_TSAN_BUILD
#define EXSIM_TSAN_BUILD 0
#endif

namespace exsim {

inline constexpr std::size_t kCacheLine = 64;
// Intel's L2 spatial prefetcher pulls cache lines in adjacent pairs, so data written by different
// threads is kept 128 bytes apart, not 64.
inline constexpr std::size_t kFalseSharingRange = 128;
inline constexpr std::uint32_t kNil = 0xFFFF'FFFFu;

using OrderId = std::uint64_t;
using Price = std::int64_t;
// 64-bit: crypto quantities are integer satoshis (1e-8 BTC), and a 32-bit count overflows at 42.9 BTC.
// Real Coinbase flow contains larger orders, so a narrower type would be incorrect, not just limiting.
using Qty = std::uint64_t;
using SymbolId = std::uint32_t;
using OwnerId = std::uint32_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class OrdType : std::uint8_t { Limit = 0, Market = 1 };
enum class Tif : std::uint8_t { Day = 0, Ioc = 1, Fok = 2 };
enum class MsgType : std::uint8_t { NewOrder = 1, Cancel = 2, Modify = 3 };
inline constexpr std::uint8_t kFlagPostOnly = 0x01;

// Self-trade prevention, applied when an incoming order would match a resting order of the same owner.
// Real venues let each order choose (Coinbase, Binance and CME all do): the book's mode is the default, and
// an order can override it through bits 1-2 of Command::flags (see stp_flag / effective_stp).
enum class Stp : std::uint8_t {
  None = 0,             // allow self-trades (faithful replay of external flow)
  CancelResting = 1,    // cancel the resting order, keep matching the incoming one ("cancel oldest")
  CancelIncoming = 2,   // stop matching, cancel the incoming order's remainder ("cancel newest")
  DecrementCancel = 3,  // cancel the smaller of the two and decrement the larger by its size; cancel both
                        // if equal (Coinbase's default "dc")
};
inline constexpr std::uint8_t kStpShift = 1;
inline constexpr std::uint8_t kStpMask = 0x06;
constexpr std::uint8_t stp_flag(Stp s) noexcept { return static_cast<std::uint8_t>(static_cast<std::uint8_t>(s) << kStpShift); }
constexpr Stp effective_stp(std::uint8_t flags, Stp book_default) noexcept {
  const auto v = static_cast<std::uint8_t>((flags & kStpMask) >> kStpShift);
  return v != 0 ? static_cast<Stp>(v) : book_default;
}

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

// A decoded inbound request. Trivially copyable so it can travel through SPSC rings by value.
struct Command {
  OrderId order_id;
  Price price;         // limit price (NewOrder) or new price (Modify)
  std::uint64_t seq;   // wire sequence number
  std::uint64_t ts;    // ingress timestamp in TSC ticks; instrumentation only, never affects matching
  Qty qty;             // order quantity (NewOrder) or new open quantity (Modify)
  OwnerId owner;
  SymbolId symbol;
  MsgType type;
  Side side;
  OrdType ord_type;
  Tif tif;
  std::uint8_t flags;
  std::uint8_t pad[3];
};
static_assert(sizeof(Command) == 56);
static_assert(std::is_trivially_copyable_v<Command>);

enum class EventType : std::uint8_t { Accepted = 1, Rejected = 2, Trade = 3, Canceled = 4, Modified = 5 };

enum class Reason : std::uint8_t {
  None = 0,
  InvalidOrderId,
  InvalidQty,
  InvalidPrice,
  DuplicateOrderId,
  UnknownOrder,
  UnknownSymbol,
  WouldCross,       // post-only order would have taken liquidity
  BookFull,         // order store exhausted; remainder cannot rest
  UserCanceled,
  IocExpired,       // IOC / market remainder
  FokUnfilled,
  SelfTrade,
  RiskMaxQty,       // pre-trade risk (risk.hpp)
  RiskMaxNotional,
  RiskCollar,       // price too far from the last trade
  RiskRateLimit,
  RiskHalted,
  UnknownOwner,
};

// Outbound execution event. Every field is named (no implicit padding), so two events are equal
// exactly when their bytes are equal. Digests and differential tests depend on this.
struct Event {
  OrderId order_id;   // Trade: the aggressor (taker)
  OrderId maker_id;   // Trade: the resting order; otherwise 0
  Price price;
  Qty qty;            // Accepted: order qty; Trade: fill qty; Modified: new qty; Canceled: canceled qty
  Qty leaves;         // Trade: maker's remaining qty after the fill
  SymbolId symbol;
  EventType type;
  Reason reason;
  Side side;          // Trade: the aggressor's side
  MsgType request;    // Rejected: which request type was rejected
};
static_assert(sizeof(Event) == 48);
static_assert(std::has_unique_object_representations_v<Event>);

constexpr Event accepted_event(SymbolId sym, OrderId id, Side side, Price px, Qty qty) noexcept {
  Event e{};
  e.type = EventType::Accepted, e.symbol = sym, e.order_id = id, e.side = side, e.price = px, e.qty = qty;
  return e;
}
constexpr Event rejected_event(const Command& c, Reason r) noexcept {
  Event e{};
  e.type = EventType::Rejected, e.symbol = c.symbol, e.order_id = c.order_id, e.side = c.side;
  e.request = c.type, e.reason = r;
  return e;
}
constexpr Event trade_event(SymbolId sym, OrderId taker, OrderId maker, Side taker_side, Price px, Qty qty,
                            Qty maker_leaves) noexcept {
  Event e{};
  e.type = EventType::Trade, e.symbol = sym, e.order_id = taker, e.maker_id = maker, e.side = taker_side;
  e.price = px, e.qty = qty, e.leaves = maker_leaves;
  return e;
}
constexpr Event canceled_event(SymbolId sym, OrderId id, Side side, Price px, Qty qty, Reason r) noexcept {
  Event e{};
  e.type = EventType::Canceled, e.symbol = sym, e.order_id = id, e.side = side, e.price = px, e.qty = qty;
  e.reason = r;
  return e;
}
constexpr Event modified_event(SymbolId sym, OrderId id, Side side, Price px, Qty qty) noexcept {
  Event e{};
  e.type = EventType::Modified, e.symbol = sym, e.order_id = id, e.side = side, e.price = px, e.qty = qty;
  return e;
}
// Decrement-and-cancel self-trade prevention reduced an order to `new_qty` without a trade.
constexpr Event stp_decrement_event(SymbolId sym, OrderId id, Side side, Price px, Qty new_qty) noexcept {
  Event e = modified_event(sym, id, side, px, new_qty);
  e.reason = Reason::SelfTrade;
  return e;
}

struct BookConfig {
  Price min_price = 0;                 // price of level 0; the book accepts [min_price, min_price + num_levels)
  std::uint32_t num_levels = 1u << 16; // price band width in ticks; multiple of 64
  std::uint32_t max_orders = 1u << 18; // resting-order capacity, preallocated
  Stp stp = Stp::None;
};

struct OrderView {
  Price price;
  Qty qty;
  Side side;
  OwnerId owner;
};

}  // namespace exsim
