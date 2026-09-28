// exsim_mmsim: runs a market-making strategy against a replayed real exchange capture.
//
//   exsim_mmsim --in btcusdt.exmd --strategy as|fixed|touch [options] [--out-prefix results/run]
//
//   --fill queue|optimistic   queue-position fill model, or the naive "touch means filled" model
//   --size LOTS               quote size per side (1 lot = 1e-5 BTC)        [1000]
//   --max-inv UNITS           inventory cap in multiples of the quote size  [10]
//   --gamma G --k K --tau S   Avellaneda-Stoikov parameters                 [0.1, 0.5, 30]
//   --half-spread TICKS       fixed-spread strategy half spread             [2]
//   --requote-ticks T         move a live quote only if its target moved >= T ticks   [2]
//   --latency-ms MS           decision-to-market latency                    [10]
//   --fee-bps B               fee per fill on notional; negative = rebate   [0]
//   --cancel-power N          cancellation split ahead/behind our quote: 1 proportional, 3 power rule  [1]
//   --json                    print one machine-readable line and nothing else
//   --calibrate FILE          instead of simulating, write trade depth data (for estimating k)
//
// Outputs (with --out-prefix P): P.fills.csv, P.series.csv (1 s samples).

#include <cstdio>
#include <fstream>
#include <iomanip>

#include "args.hpp"
#include "exsim/mm_sim.hpp"

using namespace exsim;

namespace {

void calibrate(const md::Capture& cap, const std::string& path) {
  const auto& recs = cap.records();
  std::vector<std::size_t> diffs, snaps;
  for (std::size_t i = 0; i < recs.size(); ++i) {
    if (recs[i].kind == md::Kind::Diff) diffs.push_back(i);
    else if (recs[i].kind == md::Kind::Snapshot) snaps.push_back(i);
  }
  md::Seed seed{};
  if (!md::find_seed(recs, diffs, snaps, 0, seed)) tools::Args::die("no usable seed");
  const md::Record& s0 = recs[seed.snap_rec];
  BookMirror mirror((s0.bid(0).px + s0.ask(0).px) / 2);
  mirror.seed(s0);
  std::ofstream out(path);
  out << "t_s,dist_ticks,qty_lots,aggressor,spread_ticks\n";
  const std::uint64_t t0 = recs[seed.diff_rec].rx_ns;
  for (std::size_t i = seed.diff_rec; i < recs.size(); ++i) {
    const md::Record& r = recs[i];
    if (r.kind == md::Kind::Diff) {
      mirror.apply(r);
    } else if (r.kind == md::Kind::Trade) {
      const double bb = static_cast<double>(*mirror.book().best_bid()), ba = static_cast<double>(*mirror.book().best_ask());
      const double mid = 0.5 * (bb + ba);
      // depth of the resting order that was hit, measured from mid (>= half the spread)
      const double dist = std::abs(static_cast<double>(r.trade_px) - mid);
      out << static_cast<double>(r.rx_ns - t0) * 1e-9 << ',' << dist << ',' << r.trade_qty << ','
          << (r.buyer_is_maker ? -1 : 1) << ',' << (ba - bb) << '\n';
    }
  }
}

void print_json(const mm::Result& R, const char* name) {
  std::printf(
      "{\"strategy\":\"%s\",\"fill\":\"%s\",\"gamma\":%g,\"k\":%g,\"tau\":%g,\"half_spread\":%g,\"latency_ms\":%g,"
      "\"fee_bps\":%g,\"cancel_power\":%g,\"size_lots\":%llu,\"duration_s\":%.1f,\"fills\":%llu,\"buys\":%llu,\"sells\":%llu,"
      "\"volume_btc\":%.6f,\"notional\":%.2f,\"spread_capture\":%.4f,\"inventory_pnl\":%.4f,\"fees\":%.4f,"
      "\"total_pnl\":%.4f,\"max_abs_inv\":%.5f,\"mean_abs_inv\":%.5f,\"final_inv\":%.5f,\"edge_ticks\":%.4f,"
      "\"markout1\":%.4f,\"markout5\":%.4f,\"markout30\":%.4f,\"realized1\":%.4f,\"realized5\":%.4f,\"realized30\":%.4f,"
      "\"wait_s\":%.3f,\"live_frac\":%.4f,\"cancels\":%llu,\"post_only_rejects\":%llu,\"gaps\":%llu}\n",
      name, R.cfg.fill == mm::FillModel::Queue ? "queue" : "optimistic", R.cfg.gamma, R.cfg.k, R.cfg.tau_s,
      R.cfg.half_spread_ticks, static_cast<double>(R.cfg.latency_ns) * 1e-6, R.cfg.fee_bps, R.cfg.cancel_power,
      static_cast<unsigned long long>(R.cfg.size_lots), R.duration_s, static_cast<unsigned long long>(R.fills),
      static_cast<unsigned long long>(R.buys), static_cast<unsigned long long>(R.sells), R.volume_base, R.notional,
      R.spread_capture, R.inventory_pnl, R.fees, R.total_pnl, R.max_abs_inv, R.mean_abs_inv, R.final_inv, R.avg_edge_ticks,
      R.markout_ticks[0], R.markout_ticks[1], R.markout_ticks[2], R.realized_ticks[0], R.realized_ticks[1],
      R.realized_ticks[2], R.mean_queue_wait_s, R.both_sides_live_frac, static_cast<unsigned long long>(R.cancels),
      static_cast<unsigned long long>(R.post_only_rejects), static_cast<unsigned long long>(R.gaps));
}

}  // namespace

int main(int argc, char** argv) {
  const tools::Args args(argc, argv);
  if (!args.has("in")) tools::Args::die("--in <file.exmd> is required");
  md::Capture cap(args.str("in", ""));
  if (args.has("calibrate")) {
    calibrate(cap, args.str("calibrate", ""));
    return 0;
  }

  mm::Config c;
  const std::string strat = args.str("strategy", "as");
  if (strat == "as") c.strategy = mm::StrategyKind::AvellanedaStoikov;
  else if (strat == "fixed") c.strategy = mm::StrategyKind::FixedSpread;
  else if (strat == "touch") c.strategy = mm::StrategyKind::JoinTouch;
  else tools::Args::die("--strategy must be as, fixed or touch");
  const std::string fill = args.str("fill", "queue");
  if (fill == "queue") c.fill = mm::FillModel::Queue;
  else if (fill == "optimistic") c.fill = mm::FillModel::Optimistic;
  else tools::Args::die("--fill must be queue or optimistic");
  c.size_lots = args.u64("size", c.size_lots);
  c.max_inventory_units = args.f64("max-inv", c.max_inventory_units);
  c.gamma = args.f64("gamma", c.gamma);
  c.k = args.f64("k", c.k);
  c.tau_s = args.f64("tau", c.tau_s);
  c.half_spread_ticks = args.f64("half-spread", c.half_spread_ticks);
  c.requote_ticks = static_cast<std::int64_t>(args.u64("requote-ticks", static_cast<std::uint64_t>(c.requote_ticks)));
  c.latency_ns = static_cast<std::uint64_t>(args.f64("latency-ms", 10) * 1e6);
  c.fee_bps = args.f64("fee-bps", c.fee_bps);
  c.cancel_power = args.f64("cancel-power", c.cancel_power);
  c.warmup_ns = static_cast<std::uint64_t>(args.f64("warmup-s", 30) * 1e9);

  const mm::Result R = mm::run(cap, c);
  const char* name = strat == "as" ? "as" : strat == "fixed" ? "fixed" : "touch";

  if (args.has("out-prefix")) {
    const std::string p = args.str("out-prefix", "");
    std::ofstream f(p + ".fills.csv");
    f << std::setprecision(15);  // the default 6 significant digits would quantize an 8.4e6-tick price onto a 10-tick grid
    f << "t_s,side,px_ticks,qty_lots,mid_ticks,queue_wait_s\n";
    const std::uint64_t t0 = R.fill_log.empty() ? 0 : R.fill_log.front().t;
    for (const auto& x : R.fill_log)
      f << static_cast<double>(x.t - t0) * 1e-9 << ',' << (x.side == Side::Buy ? "buy" : "sell") << ',' << x.px << ','
        << x.qty << ',' << x.mid_at_fill << ',' << x.queue_wait_s << '\n';
    std::ofstream s(p + ".series.csv");
    s << std::setprecision(15);
    s << "t_s,mid_ticks,inv_btc,pnl_usdt\n";
    for (const auto& x : R.series) s << x.t_s << ',' << x.mid << ',' << x.inv_btc << ',' << x.pnl_usdt << '\n';
  }

  if (args.has("json")) {
    print_json(R, name);
    return 0;
  }
  std::printf("%s (%s fills, latency %.0f ms, fee %.2f bps), %.0f s of quoting\n", name, fill.c_str(),
              static_cast<double>(c.latency_ns) * 1e-6, c.fee_bps, R.duration_s);
  std::printf("  fills %llu (%llu buys / %llu sells), volume %.4f BTC, notional %.0f USDT\n",
              static_cast<unsigned long long>(R.fills), static_cast<unsigned long long>(R.buys),
              static_cast<unsigned long long>(R.sells), R.volume_base, R.notional);
  std::printf("  PnL %.4f USDT = spread capture %.4f + inventory %.4f - fees %.4f\n", R.total_pnl, R.spread_capture,
              R.inventory_pnl, R.fees);
  std::printf("  inventory: mean |q| %.4f BTC, max |q| %.4f BTC, final %.4f BTC\n", R.mean_abs_inv, R.max_abs_inv, R.final_inv);
  std::printf("  edge at fill %.2f ticks; markout (mid drift after fill, in our favor): 1s %.1f, 5s %.1f, 30s %.1f ticks\n",
              R.avg_edge_ticks, R.markout_ticks[0], R.markout_ticks[1], R.markout_ticks[2]);
  std::printf("  mean queue wait %.2f s, both sides live %.0f%% of the time, %llu cancels, %llu post-only rejects\n",
              R.mean_queue_wait_s, 100 * R.both_sides_live_frac, static_cast<unsigned long long>(R.cancels),
              static_cast<unsigned long long>(R.post_only_rejects));
  return R.gaps == 0 ? 0 : 3;
}
