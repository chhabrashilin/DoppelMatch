// Market-making simulator on top of a replayed exchange book.
//
// Our quotes are VIRTUAL. They are never inserted into the mirrored book: the exchange's level
// quantities are absolute totals, so inserting our size would double-count it against the next update.
// Instead each quote tracks its own estimated position in the exchange's FIFO queue, driven only by
// observable flow (trades and level-quantity changes).
//
// Fill model (FillModel::Queue), for a resting BID at price P (asks are symmetric):
//   * On becoming live we join the BACK of the queue: queue_ahead = displayed quantity at P.
//   * A sell-aggressor trade at P consumes the queue ahead of us first; only the excess fills us.
//   * A sell-aggressor trade BELOW P swept through our price (every bid at P and above was
//     exhausted), so we are filled in full. A trade above P never reached us.
//   * Level shrinkage that trades do not explain is cancellation. A canceller's queue position is not
//     observable, so it is split by a rule: proportional to the queue ahead of and behind us by default
//     (Config::cancel_power = 1), or by a power rule (cancel_power = 3, which L3 ground truth supports).
//   * Level growth joins behind us and never improves our position.
//   * If the opposite best price moves to or through our price, we are filled in full.
// FillModel::Optimistic ignores the queue (any trade at or through our price fills us). It exists to
// measure how much a naive backtest overstates results.
//
// Not modeled (stated, not hidden): our own market impact, the exchange's matching-engine latency
// jitter, and queue-jumping by hidden/iceberg orders.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "exsim/common.hpp"

namespace exsim::mm {

enum class StrategyKind { AvellanedaStoikov, FixedSpread, JoinTouch };
enum class FillModel { Queue, Optimistic };

struct Config {
  StrategyKind strategy = StrategyKind::AvellanedaStoikov;
  FillModel fill = FillModel::Queue;
  std::uint64_t size_lots = 1000;      // quote size per side
  double max_inventory_units = 10;     // in multiples of size_lots; quoting a side stops at the cap
  // Avellaneda-Stoikov
  double gamma = 0.1;                  // risk aversion
  double k = 0.5;                      // order-arrival decay per tick of distance from mid
  double tau_s = 30;                   // fixed horizon (continuous operation: constant, not T - t)
  // fixed spread
  double half_spread_ticks = 2;
  // shared
  double vol_halflife_s = 10;
  std::int64_t requote_ticks = 2;         // move a live quote only if the target shifted by at least this (not JoinTouch)
  std::uint64_t latency_ns = 10'000'000;  // decision -> live, and decision -> cancel effective
  double fee_bps = 0;                     // per fill on notional; negative = rebate
  double cancel_power = 1;                // how cancellations split ahead/behind us (1 = proportional; see QuoteLogic)
  std::uint64_t warmup_ns = 30'000'000'000ull;
  double price_scale = 0.01;              // quote-currency per tick
  double qty_scale = 1e-5;                // base units per lot
};

struct Fill {
  std::uint64_t t;
  Side side;
  std::int64_t px;
  std::uint64_t qty;
  double mid_at_fill;                     // ticks
  double queue_wait_s;                    // time from going live to this fill
};

// One resting virtual quote (at most one per side).
struct Quote {
  bool active = false;
  std::int64_t px = 0;
  std::uint64_t remaining = 0;
  double ahead = 0;                       // lots ahead of us in the exchange queue
  double traded_since_diff = 0;           // lots traded at our price since the last book update
  std::uint64_t live_since = 0;

  // pending replace: cancel `active` (if any) and place `pend_px` (if any), both effective at `eff`
  bool has_pending = false;
  std::optional<std::int64_t> pend_px;
  std::uint64_t eff = 0;
};

// Pure quote-state transitions, separated from the simulation loop so they can be tested in isolation.
struct QuoteLogic {
  // A trade of `qty` lots at `trade_px` whose aggressor hits our side (a sell for our bid, a buy for
  // our ask). Returns the lots filled.
  //   "through": the aggressor swept past our price (trade below our bid / above our ask), so every
  //              order at our price was exhausted, including ours.
  //   "at":      the trade is at our price. It consumes the queue ahead of us first.
  //   otherwise: the trade never reached our price.
  static std::uint64_t on_trade(Quote& q, Side side, std::int64_t trade_px, std::uint64_t qty, FillModel model) {
    if (!q.active || q.remaining == 0) return 0;
    const bool through = side == Side::Buy ? trade_px < q.px : trade_px > q.px;
    const bool at = trade_px == q.px;
    if (!through && !at) return 0;
    std::uint64_t fill;
    if (through) {
      fill = q.remaining;
    } else if (model == FillModel::Optimistic) {
      fill = std::min<std::uint64_t>(q.remaining, qty);
    } else {
      const double consumed = std::min(q.ahead, static_cast<double>(qty));
      q.ahead -= consumed;
      q.traded_since_diff += static_cast<double>(qty);
      fill = static_cast<std::uint64_t>(std::min(static_cast<double>(qty) - consumed, static_cast<double>(q.remaining)));
    }
    q.remaining -= fill;
    if (q.remaining == 0) q.active = false;
    return fill;
  }

  // Exchange level at our price changed from `before` to `after` lots at a book update. `power` sets how a
  // cancellation is split between the queue ahead of us (a) and behind us (b): share ahead =
  // a^n / (a^n + b^n). n = 1 is proportional; larger n concentrates cancellations on the larger part of the
  // queue. Scored against Coinbase's order-by-order ground truth, n = 3 fits best (docs/RESEARCH.md, part 2).
  static void on_level_change(Quote& q, double before, double after, double power = 1.0) {
    if (!q.active) return;
    const double shrink = before - after;
    if (shrink > 0) {
      const double explained = std::min(shrink, q.traded_since_diff);
      const double cancelled = shrink - explained;
      const double base = before - explained;  // level after trades, before cancellations
      if (cancelled > 0 && base > 0) {
        const double a = std::min(q.ahead, base), b = base - a;
        const double an = std::pow(a, power), bn = std::pow(b, power);
        const double share = an + bn > 0 ? an / (an + bn) : 0.0;
        q.ahead -= std::min(a, cancelled * share);
      }
    }
    q.ahead = std::clamp(q.ahead, 0.0, std::max(after, 0.0));
    q.traded_since_diff = 0;
  }
};

struct Strategy {
  // Desired quote prices in ticks, or nullopt for "do not quote this side".
  struct Desired {
    std::optional<std::int64_t> bid, ask;
  };

  static Desired desired(const Config& c, double mid, std::int64_t best_bid, std::int64_t best_ask, double inv_units,
                         double sigma2_ticks2_per_s) {
    double bid_f = 0, ask_f = 0;
    switch (c.strategy) {
      case StrategyKind::AvellanedaStoikov: {
        const double s2 = sigma2_ticks2_per_s;
        const double r = mid - inv_units * c.gamma * s2 * c.tau_s;
        const double half = 0.5 * c.gamma * s2 * c.tau_s + std::log1p(c.gamma / c.k) / c.gamma;
        bid_f = std::floor(r - half), ask_f = std::ceil(r + half);
        break;
      }
      case StrategyKind::FixedSpread:
        bid_f = std::floor(mid - c.half_spread_ticks), ask_f = std::ceil(mid + c.half_spread_ticks);
        break;
      case StrategyKind::JoinTouch:
        bid_f = static_cast<double>(best_bid), ask_f = static_cast<double>(best_ask);
        break;
    }
    auto bid = static_cast<std::int64_t>(bid_f), ask = static_cast<std::int64_t>(ask_f);
    bid = std::min(bid, best_ask - 1);  // never cross: we only ever provide liquidity
    ask = std::max(ask, best_bid + 1);
    Desired d;
    if (inv_units < c.max_inventory_units) d.bid = bid;
    if (inv_units > -c.max_inventory_units) d.ask = ask;
    return d;
  }
};

// Exponentially weighted variance rate of mid changes, robust to irregular sampling.
class VolEstimator {
 public:
  explicit VolEstimator(double halflife_s) : hl_(halflife_s) {}
  void update(double mid, std::uint64_t t_ns) {
    if (have_) {
      const double dt = static_cast<double>(t_ns - last_t_) * 1e-9;
      if (dt > 0) {
        const double a = 1.0 - std::exp(-dt * 0.693147 / hl_);
        const double dm = mid - last_mid_;
        var_ += a * (dm * dm / dt - var_);
      }
    }
    last_mid_ = mid, last_t_ = t_ns, have_ = true;
  }
  double variance_rate() const { return var_; }

 private:
  double hl_, var_ = 1.0, last_mid_ = 0;  // prior: 1 tick^2/s until data arrives
  std::uint64_t last_t_ = 0;
  bool have_ = false;
};

}  // namespace exsim::mm
