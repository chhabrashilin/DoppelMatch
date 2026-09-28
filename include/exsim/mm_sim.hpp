// The simulation loop: replays a capture, runs one strategy against it, and accounts for PnL.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "exsim/md.hpp"
#include "exsim/md_mirror.hpp"
#include "exsim/mm.hpp"

namespace exsim::mm {

struct SeriesPoint {
  double t_s;       // seconds since the first quote-eligible moment
  double mid;       // ticks
  double inv_btc;
  double pnl_usdt;  // mark-to-market, net of fees
};

struct Result {
  Config cfg;
  double duration_s = 0;
  std::uint64_t fills = 0, buys = 0, sells = 0;
  double volume_base = 0, notional = 0;
  double spread_capture = 0;   // sum of side*(mid_at_fill - price)*qty, in quote currency
  double inventory_pnl = 0;    // mark-to-market drift of held inventory (residual)
  double fees = 0;
  double total_pnl = 0;        // = spread_capture + inventory_pnl - fees
  double max_abs_inv = 0, mean_abs_inv = 0, final_inv = 0;
  double avg_edge_ticks = 0;   // mean side*(mid_at_fill - price) at the moment of the fill
  double markout_ticks[3] = {0, 0, 0};   // side*(mid(t+h) - mid(t)) at h = 1, 5, 30 s
  double realized_ticks[3] = {0, 0, 0};  // side*(mid(t+h) - price)
  std::uint64_t markout_n[3] = {0, 0, 0};
  std::uint64_t quotes_live = 0, cancels = 0, post_only_rejects = 0;
  double both_sides_live_frac = 0;
  double mean_queue_wait_s = 0;
  std::uint64_t gaps = 0, reseeds = 0;
  std::vector<Fill> fill_log;
  std::vector<SeriesPoint> series;   // 1 s samples
};

inline Result run(const md::Capture& cap, const Config& cfg) {
  const auto& recs = cap.records();
  std::vector<std::size_t> diffs, snaps;
  for (std::size_t i = 0; i < recs.size(); ++i) {
    if (recs[i].kind == md::Kind::Diff) diffs.push_back(i);
    else if (recs[i].kind == md::Kind::Snapshot) snaps.push_back(i);
  }
  md::Seed seed{};
  if (diffs.empty() || snaps.empty() || !md::find_seed(recs, diffs, snaps, 0, seed))
    throw std::runtime_error("capture has no usable snapshot/diff pair");
  std::unordered_map<std::uint64_t, std::size_t> snap_by_id;
  for (std::size_t s : snaps) snap_by_id[recs[s].last_id] = s;
  const md::Record& s0 = recs[seed.snap_rec];
  BookMirror mirror((s0.bid(0).px + s0.ask(0).px) / 2);
  mirror.seed(s0);

  Result R;
  R.cfg = cfg;
  const double ps = cfg.price_scale, qs_ = cfg.qty_scale;
  VolEstimator vol(cfg.vol_halflife_s);
  Quote quote[2];  // [0] = our bid, [1] = our ask
  double cash = 0, fees = 0, spread_capture = 0;
  std::int64_t inv = 0;
  std::vector<std::pair<std::uint64_t, double>> mids;  // (t, mid ticks), every diff
  double wait_sum = 0;
  double abs_inv_sum = 0;
  std::uint64_t abs_inv_n = 0;
  std::uint64_t prev_last = 0, t_prev = 0, t_start = 0, live_ns = 0, span_ns = 0, next_sample = 0;
  bool have_prev = false;

  auto mid_now = [&] { return 0.5 * static_cast<double>(*mirror.book().best_bid() + *mirror.book().best_ask()); };
  auto best_bid = [&] { return *mirror.book().best_bid(); };
  auto best_ask = [&] { return *mirror.book().best_ask(); };

  auto record_fill = [&](Side our_side, std::int64_t px, std::uint64_t qty, std::uint64_t t, const Quote& q) {
    if (qty == 0) return;
    const double sign = our_side == Side::Buy ? 1.0 : -1.0;
    const double notional = static_cast<double>(px) * ps * static_cast<double>(qty) * qs_;
    cash -= sign * notional;
    fees += cfg.fee_bps * 1e-4 * notional;
    inv += our_side == Side::Buy ? static_cast<std::int64_t>(qty) : -static_cast<std::int64_t>(qty);
    const double mid = mid_now();
    spread_capture += sign * (mid - static_cast<double>(px)) * ps * static_cast<double>(qty) * qs_;
    ++R.fills;
    (our_side == Side::Buy ? R.buys : R.sells)++;
    R.volume_base += static_cast<double>(qty) * qs_;
    R.notional += notional;
    const double wait = static_cast<double>(t - q.live_since) * 1e-9;
    wait_sum += wait;
    R.fill_log.push_back({t, our_side, px, qty, mid, wait});
  };

  auto process_pending = [&](std::uint64_t t) {
    for (int i = 0; i < 2; ++i) {
      Quote& q = quote[i];
      if (!q.has_pending || q.eff > t) continue;
      const Side side = i == 0 ? Side::Buy : Side::Sell;
      if (q.active) q.active = false, ++R.cancels;
      if (q.pend_px) {
        const std::int64_t px = *q.pend_px;
        const bool crossing = side == Side::Buy ? px >= best_ask() : px <= best_bid();
        if (crossing) {
          ++R.post_only_rejects;  // it would have taken liquidity: rejected, as a post-only order is
        } else {
          q.active = true, q.px = px, q.remaining = cfg.size_lots;
          q.ahead = static_cast<double>(mirror.qty_at(side, px));
          q.traded_since_diff = 0, q.live_since = t;
          ++R.quotes_live;
        }
      }
      q.has_pending = false, q.pend_px.reset();
    }
  };

  std::size_t pos = static_cast<std::size_t>(seed.diff_rec);
  for (std::size_t i = pos; i < recs.size(); ++i) {
    const md::Record& r = recs[i];
    if (r.kind == md::Kind::Snapshot) continue;
    const std::uint64_t t = r.rx_ns;
    if (!have_prev && r.kind == md::Kind::Diff) t_start = t;
    if (t_start == 0) continue;  // trades before the first applied diff carry no book context
    process_pending(t);

    if (r.kind == md::Kind::Trade) {
      const bool sell_aggressor = r.buyer_is_maker;  // hit a resting bid
      Quote& q = sell_aggressor ? quote[0] : quote[1];
      const Side our_side = sell_aggressor ? Side::Buy : Side::Sell;
      const std::int64_t px = q.px;
      const std::uint64_t f = QuoteLogic::on_trade(q, our_side, r.trade_px, r.trade_qty, cfg.fill);
      record_fill(our_side, px, f, t, q);
      continue;
    }

    // ---- book update ----
    if (have_prev && r.first_id != prev_last + 1) {
      ++R.gaps;
      break;  // a sequence gap invalidates the mirror; we stop rather than simulate on a wrong book
    }
    double before[2] = {0, 0};
    for (int k = 0; k < 2; ++k)
      if (quote[k].active) before[k] = static_cast<double>(mirror.qty_at(k == 0 ? Side::Buy : Side::Sell, quote[k].px));
    mirror.apply(r);
    // The market has consumed half of the seeded depth: reseed exactly from the snapshot aligned to this
    // diff, so the book stays complete near the touch (see md_mirror.hpp).
    if (const auto it = snap_by_id.find(r.last_id); it != snap_by_id.end() && mirror.needs_reseed()) {
      mirror.seed(recs[it->second]);
      ++R.reseeds;
    }
    prev_last = r.last_id, have_prev = true;
    for (int k = 0; k < 2; ++k)
      if (quote[k].active)
        QuoteLogic::on_level_change(quote[k], before[k],
                                    static_cast<double>(mirror.qty_at(k == 0 ? Side::Buy : Side::Sell, quote[k].px)),
                                    cfg.cancel_power);
    // the market moved to or through us: filled in full at our price
    if (quote[0].active && quote[0].px >= best_ask()) {
      const std::uint64_t f = quote[0].remaining;
      quote[0].remaining = 0, quote[0].active = false;
      record_fill(Side::Buy, quote[0].px, f, t, quote[0]);
    }
    if (quote[1].active && quote[1].px <= best_bid()) {
      const std::uint64_t f = quote[1].remaining;
      quote[1].remaining = 0, quote[1].active = false;
      record_fill(Side::Sell, quote[1].px, f, t, quote[1]);
    }

    const double mid = mid_now();
    vol.update(mid, t);
    mids.emplace_back(t, mid);
    if (t_prev != 0) {
      span_ns += t - t_prev;
      if (quote[0].active && quote[1].active) live_ns += t - t_prev;
    }
    t_prev = t;

    const bool quoting = t >= t_start + cfg.warmup_ns;
    if (quoting) {
      if (next_sample == 0) next_sample = t;
      const double inv_units = static_cast<double>(inv) / static_cast<double>(cfg.size_lots);
      const auto want = Strategy::desired(cfg, mid, best_bid(), best_ask(), inv_units, vol.variance_rate());
      const std::optional<std::int64_t> wants[2] = {want.bid, want.ask};
      for (int k = 0; k < 2; ++k) {
        Quote& q = quote[k];
        const std::optional<std::int64_t> current =
            q.has_pending ? q.pend_px : (q.active ? std::optional<std::int64_t>(q.px) : std::nullopt);
        if (wants[k] == current) continue;
        // hysteresis: re-queueing forfeits queue position, so ignore small target moves
        const std::int64_t thr = cfg.strategy == StrategyKind::JoinTouch ? 1 : cfg.requote_ticks;
        if (wants[k] && current && std::abs(*wants[k] - *current) < thr) continue;
        if (q.has_pending) {
          q.pend_px = wants[k];  // amend in flight: keeps the original effective time
        } else {
          q.has_pending = true, q.pend_px = wants[k], q.eff = t + cfg.latency_ns;
        }
      }
      abs_inv_sum += std::abs(static_cast<double>(inv)) * qs_, ++abs_inv_n;
      R.max_abs_inv = std::max(R.max_abs_inv, std::abs(static_cast<double>(inv)) * qs_);
      if (t >= next_sample) {
        const double pnl = cash + static_cast<double>(inv) * qs_ * mid * ps - fees;
        R.series.push_back({static_cast<double>(t - (t_start + cfg.warmup_ns)) * 1e-9, mid,
                            static_cast<double>(inv) * qs_, pnl});
        next_sample += 1'000'000'000ull;
      }
    }
  }

  const double mid_end = mids.empty() ? 0 : mids.back().second;
  R.duration_s = R.series.empty() ? 0 : R.series.back().t_s;
  R.fees = fees;
  R.spread_capture = spread_capture;
  R.final_inv = static_cast<double>(inv) * qs_;
  R.total_pnl = cash + static_cast<double>(inv) * qs_ * mid_end * ps - fees;
  R.inventory_pnl = R.total_pnl + fees - spread_capture;
  R.mean_abs_inv = abs_inv_n ? abs_inv_sum / static_cast<double>(abs_inv_n) : 0;
  R.mean_queue_wait_s = R.fills ? wait_sum / static_cast<double>(R.fills) : 0;
  R.both_sides_live_frac = span_ns ? static_cast<double>(live_ns) / static_cast<double>(span_ns) : 0;

  // Fill-conditional statistics.
  double edge_sum = 0;
  const std::uint64_t horizons[3] = {1'000'000'000ull, 5'000'000'000ull, 30'000'000'000ull};
  for (const Fill& f : R.fill_log) {
    const double sign = f.side == Side::Buy ? 1.0 : -1.0;
    edge_sum += sign * (f.mid_at_fill - static_cast<double>(f.px));
    for (int h = 0; h < 3; ++h) {
      auto it = std::lower_bound(mids.begin(), mids.end(), f.t + horizons[h],
                                 [](const auto& a, std::uint64_t v) { return a.first < v; });
      if (it == mids.end()) continue;
      R.markout_ticks[h] += sign * (it->second - f.mid_at_fill);
      R.realized_ticks[h] += sign * (it->second - static_cast<double>(f.px));
      ++R.markout_n[h];
    }
  }
  if (R.fills) R.avg_edge_ticks = edge_sum / static_cast<double>(R.fills);
  for (int h = 0; h < 3; ++h)
    if (R.markout_n[h]) {
      R.markout_ticks[h] /= static_cast<double>(R.markout_n[h]);
      R.realized_ticks[h] /= static_cast<double>(R.markout_n[h]);
    }
  return R;
}

}  // namespace exsim::mm
