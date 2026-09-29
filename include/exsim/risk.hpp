// Pre-trade risk gate: sits between the sequencer and the matching engine.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "exsim/common.hpp"

namespace exsim {

struct RiskConfig {
  Qty max_qty = 1'000'000;                 // per order
  std::uint64_t max_notional = 1ull << 40; // price(ticks) * qty(lots), per order; 0 disables
  Price collar_ticks = 0;                  // reject limit orders further than this from the last trade; 0 disables
  std::uint32_t max_owners = 1u << 16;     // owner ids must be below this
  // Per-owner limit on new orders and modifies (cancels are always allowed, so a participant can
  // always reduce risk). Generic cell rate algorithm: `burst` messages may arrive at once, then
  // `rate_per_sec` sustained. 0 disables.
  std::uint64_t rate_per_sec = 0;
  std::uint64_t burst = 100;
  std::uint64_t ticks_per_sec = 1'000'000'000;  // unit of Command::ts (ns by default)
};

// Every decision is a pure function of the command stream (including Command::ts, which the journal
// records), so a journal replay reproduces risk rejections exactly. No allocation after construction.
template <class Engine>
class RiskGate {
 public:
  RiskGate(Engine& engine, const RiskConfig& cfg)
      : engine_(engine), cfg_(cfg), last_px_(engine.num_symbols(), 0), halted_(engine.num_symbols(), 0),
        tat_(cfg.max_owners, 0) {
    interval_ = cfg.rate_per_sec ? cfg.ticks_per_sec / cfg.rate_per_sec : 0;
  }

  // Stops new orders and modifies on one symbol (cancels still work) until resume().
  void halt(SymbolId s) { if (s < halted_.size()) halted_[s] = 1; }
  void resume(SymbolId s) { if (s < halted_.size()) halted_[s] = 0; }
  void halt_all(bool on) { halt_all_ = on; }
  std::uint64_t rejected() const { return rejected_; }

  template <class Sink>
  void process(const Command& c, Sink& sink) {
    if (c.type != MsgType::Cancel) {
      if (const Reason r = check(c); r != Reason::None) {
        ++rejected_;
        return sink.on_event(rejected_event(c, r));
      }
    }
    Tap<Sink> tap{sink, *this};
    engine_.process(c, tap);
  }

 private:
  template <class Sink>
  struct Tap {
    Sink& inner;
    RiskGate& g;
    void on_event(const Event& e) {
      if (e.type == EventType::Trade && e.symbol < g.last_px_.size()) g.last_px_[e.symbol] = e.price;
      inner.on_event(e);
    }
  };

  Reason check(const Command& c) {
    if (c.symbol >= last_px_.size()) return Reason::None;  // the engine reports UnknownSymbol
    if (halt_all_ || halted_[c.symbol]) return Reason::RiskHalted;
    if (c.qty > cfg_.max_qty) return Reason::RiskMaxQty;
    const bool priced = c.type == MsgType::Modify || c.ord_type == OrdType::Limit;
    // price * qty > max, tested as price > max / qty: the product itself can overflow 64 bits and wrap under the cap.
    if (priced && cfg_.max_notional != 0 && c.qty != 0 &&
        static_cast<std::uint64_t>(c.price < 0 ? 0 : c.price) > cfg_.max_notional / c.qty)
      return Reason::RiskMaxNotional;
    if (priced && cfg_.collar_ticks != 0 && last_px_[c.symbol] != 0) {
      const Price d = c.price - last_px_[c.symbol];
      if ((d < 0 ? -d : d) > cfg_.collar_ticks) return Reason::RiskCollar;
    }
    if (c.type == MsgType::NewOrder || c.type == MsgType::Modify) {
      if (c.owner >= cfg_.max_owners) return Reason::UnknownOwner;
      if (interval_ != 0) {
        std::uint64_t& tat = tat_[c.owner];
        const std::uint64_t slack = interval_ * (cfg_.burst ? cfg_.burst - 1 : 0);  // `burst` messages may arrive together
        const std::uint64_t allowed_from = tat > slack ? tat - slack : 0;
        if (c.ts < allowed_from) return Reason::RiskRateLimit;
        tat = std::max(c.ts, tat) + interval_;
      }
    }
    return Reason::None;
  }

  Engine& engine_;
  RiskConfig cfg_;
  std::vector<Price> last_px_;
  std::vector<std::uint8_t> halted_;
  std::vector<std::uint64_t> tat_;
  std::uint64_t interval_ = 0, rejected_ = 0;
  bool halt_all_ = false;
};

}  // namespace exsim
