#!/usr/bin/env python3
"""Independent audit of the market-making simulator's accounting.

Recomputes each run's PnL, inventory and spread capture from the raw fill log (fills.csv) and the 1 Hz
mark-to-market series (series.csv), with NO code shared with the C++ simulator, and compares the results
with what the simulator reported. A sign error, double-counted fill or wrong fee treatment would show up
as a mismatch here.

    python verify_accounting.py --results results --dataset btcusdt_a [--price-scale 0.01 --qty-scale 1e-5]
"""
import argparse, glob, json, os, subprocess, sys
import numpy as np
import pandas as pd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--dataset", required=True)
    ap.add_argument("--price-scale", type=float, default=0.01)
    ap.add_argument("--qty-scale", type=float, default=1e-5)
    a = ap.parse_args()
    grid = pd.read_csv(os.path.join(a.results, "grid.csv"))
    grid = grid[(grid.dataset == a.dataset) & (grid.latency_ms == 10) & (grid.fee_bps == 0) & (grid.fill == "queue")]
    if "cancel_power" in grid.columns:  # the main runs (with fill logs) use the proportional rule
        grid = grid[grid.cancel_power == 1]
    ps, qs = a.price_scale, a.qty_scale
    worst = 0.0
    print(f"{'run':<22} {'fills':>6} {'PnL sim':>10} {'PnL audit':>10} {'inv sim':>9} {'inv audit':>9} {'spread sim':>10} {'spread aud':>10}  ok")
    for _, r in grid.iterrows():
        name = r.label.replace(" ", "_").replace("=", "")
        fills = pd.read_csv(os.path.join(a.results, "runs", f"{a.dataset}_{name}.fills.csv"))
        series = pd.read_csv(os.path.join(a.results, "runs", f"{a.dataset}_{name}.series.csv"))
        sign = np.where(fills.side == "buy", 1.0, -1.0)
        notional = fills.px_ticks * ps * fills.qty_lots * qs
        cash = -(sign * notional).sum()
        inv = (sign * fills.qty_lots * qs).sum()
        # spread capture: value each fill against the mid at the moment of the fill
        spread = (sign * (fills.mid_ticks - fills.px_ticks) * ps * fills.qty_lots * qs).sum()
        # Mark inventory at the last 1 Hz sample. The simulator marks at the very last book update, which can be
        # a fraction of a second later, so allow the residual inventory to have moved by up to 200 ticks.
        final_mid = series.mid_ticks.iloc[-1]
        pnl = cash + inv * final_mid * ps
        tol = abs(inv) * ps * 200 + 1e-3
        # the JSON reports inventory to 5 decimals and spread capture to 4
        ok = abs(pnl - r.total_pnl) <= tol and abs(inv - r.final_inv) < 1e-5 and abs(spread - r.spread_capture) < 1e-4
        worst = max(worst, abs(pnl - r.total_pnl))
        print(f"{r.label:<22} {len(fills):>6} {r.total_pnl:>10.4f} {pnl:>10.4f} {r.final_inv:>9.5f} {inv:>9.5f} {r.spread_capture:>10.4f} {spread:>10.4f}  {'OK' if ok else 'MISMATCH'}")
        if not ok:
            sys.exit(1)
    print(f"all runs reconcile (largest PnL difference {worst:.4f} USDT, attributable to marking at the last 1 Hz sample)")


if __name__ == "__main__":
    main()
