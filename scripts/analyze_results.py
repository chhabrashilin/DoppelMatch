#!/usr/bin/env python3
"""Turns the experiment grid into figures and a summary table with bootstrap confidence intervals.

    python analyze_results.py --results results --img docs/img --dataset btcusdt_a

Statistics. PnL in a market-making run is a sum of highly autocorrelated increments, so a naive standard
error would be far too small. We use a MOVING-BLOCK bootstrap on 30-second PnL increments: resample blocks
with replacement and re-sum. For a difference between two strategies the blocks are PAIRED (the same
time windows are drawn for both), which cancels the market-wide component of the noise. With ~1 hour of
data the intervals are wide, and this script reports that plainly rather than hiding it.
"""
import argparse, json, os
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# Okabe-Ito: distinguishable for common color-vision deficiencies
COL = {"touch": "#666666", "fixed": "#0072B2", "as": "#D55E00", "spread": "#009E73", "inv": "#CC79A7", "fees": "#999999"}
plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False, "axes.grid": True,
                     "grid.alpha": 0.25, "figure.dpi": 130, "savefig.bbox": "tight"})
BLOCK_S = 30
rng = np.random.default_rng(12345)


def kind(label):
    return "as" if label.startswith("as") else "fixed" if label.startswith("fixed") else "touch"


def block_increments(series_csv):
    s = pd.read_csv(series_csv)
    if len(s) < 2 * BLOCK_S:
        return np.array([])
    pnl = s.pnl_usdt.to_numpy()
    idx = np.arange(0, len(pnl), BLOCK_S)
    edges = np.append(pnl[idx], pnl[-1])
    return np.diff(edges)


def boot_ci(inc, n=4000):
    if len(inc) < 3:
        return (np.nan, np.nan)
    draws = rng.integers(0, len(inc), size=(n, len(inc)))
    tot = inc[draws].sum(axis=1)
    return tuple(np.percentile(tot, [2.5, 97.5]))


def paired_boot_ci(a, b, n=4000):
    m = min(len(a), len(b))
    d = a[:m] - b[:m]
    if m < 3:
        return (np.nan, np.nan)
    draws = rng.integers(0, m, size=(n, m))
    tot = d[draws].sum(axis=1)
    return tuple(np.percentile(tot, [2.5, 97.5]))


def run_series(results, ds, label):
    name = label.replace(" ", "_").replace("=", "")
    return os.path.join(results, "runs", f"{ds}_{name}.series.csv")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--img", required=True)
    ap.add_argument("--dataset", required=True)
    ap.add_argument("--tag", default=None, help="suffix for output names (default: dataset)")
    a = ap.parse_args()
    ds, tag = a.dataset, a.tag or a.dataset
    os.makedirs(a.img, exist_ok=True)
    g = pd.read_csv(os.path.join(a.results, "grid.csv"))
    g = g[g.dataset == ds].copy()
    if "cancel_power" not in g.columns:
        g["cancel_power"] = 1.0
    alt_rule = g[g.cancel_power != 1].copy()  # the power-3 cancellation rule, compared at the end
    g = g[g.cancel_power == 1].copy()
    g["kind"] = g.label.map(kind)
    order = ["touch"] + [l for l in g.label.unique() if l.startswith("fixed")] + [l for l in g.label.unique() if l.startswith("as")]
    main_ = g[(g.latency_ms == 10) & (g.fee_bps == 0) & (g.fill == "queue")].set_index("label").loc[order]

    # ---------------- summary table with bootstrap CIs ----------------
    inc = {l: block_increments(run_series(a.results, ds, l)) for l in order}
    rows = []
    for l in order:
        r = main_.loc[l]
        lo, hi = boot_ci(inc[l])
        rows.append({"strategy": l, "fills": int(r.fills), "volume_base": r.volume_btc, "notional_usdt": r.notional,
                     "pnl_bps_of_notional": 1e4 * r.total_pnl / r.notional if r.notional else float("nan"), "spread_capture": r.spread_capture,
                     "inventory_pnl": r.inventory_pnl, "total_pnl": r.total_pnl, "ci_lo": lo, "ci_hi": hi,
                     "mean_abs_inv": r.mean_abs_inv, "markout1": r.markout1, "markout5": r.markout5, "markout30": r.markout30,
                     "wait_s": r.wait_s, "cancels": int(r.cancels)})
    tbl = pd.DataFrame(rows)
    tbl.to_csv(os.path.join(a.results, f"summary_{tag}.csv"), index=False)

    fixed_labels = [l for l in order if l.startswith("fixed")]
    as_labels = [l for l in order if l.startswith("as")]
    k_used = float(g.k.iloc[0])
    ref_fixed = f"fixed h={round(1 / k_used)}"  # a fixed spread as wide as A-S's own quotes (1/k ticks): the fair control
    assert ref_fixed in fixed_labels, (ref_fixed, fixed_labels)
    diffs = {}
    for l in order:
        if l.startswith("as"):
            lo, hi = paired_boot_ci(inc[l], inc[ref_fixed])
            diffs[l] = {"vs": ref_fixed, "mean": float(inc[l][:len(inc[ref_fixed])].sum() - inc[ref_fixed][:len(inc[l])].sum()), "ci_lo": lo, "ci_hi": hi}
        if l != "touch" and not l.startswith("as"):
            pass
    with open(os.path.join(a.results, f"paired_{tag}.json"), "w") as f:
        json.dump(diffs, f, indent=2)

    # ---------------- Figure 1: PnL decomposition ----------------
    fig, ax = plt.subplots(figsize=(8.6, 4.2))
    x = np.arange(len(order))
    ax.bar(x - 0.27, tbl.spread_capture, 0.27, color=COL["spread"], label="spread capture")
    ax.bar(x, tbl.inventory_pnl, 0.27, color=COL["inv"], label="inventory PnL")
    ax.bar(x + 0.27, tbl.total_pnl, 0.27, color=[COL[kind(l)] for l in order], label="total (colored by strategy)")
    ax.errorbar(x + 0.27, tbl.total_pnl, yerr=[tbl.total_pnl - tbl.ci_lo, tbl.ci_hi - tbl.total_pnl], fmt="none", ecolor="black", capsize=3, lw=1)
    ax.axhline(0, color="black", lw=0.8)
    ax.set_xticks(x)
    ax.set_xticklabels(order, rotation=25, ha="right")
    ax.set_ylabel("USDT over the session")
    ax.set_title(f"{ds}: where the PnL comes from (10 ms latency, no fees; 95% block-bootstrap CI on total)")
    ax.legend(frameon=False, fontsize=8)
    fig.savefig(os.path.join(a.img, f"mm_pnl_decomposition_{tag}.png"))
    plt.close(fig)

    # ---------------- Figure 2: latency ----------------
    fig, ax = plt.subplots(figsize=(6.6, 4.2))
    for l in order:
        d = g[(g.label == l) & (g.fee_bps == 0) & (g.fill == "queue")].sort_values("latency_ms")
        ax.plot(d.latency_ms, d.total_pnl, marker="o", ms=4, lw=1.4, color=COL[kind(l)], alpha=0.35 + 0.65 * (kind(l) == "as"), label=l)
    ax.set_xlabel("decision-to-market latency (ms)")
    ax.set_ylabel("total PnL (USDT)")
    ax.set_title(f"{ds}: sensitivity to latency")
    ax.legend(frameon=False, fontsize=7, ncol=2)
    fig.savefig(os.path.join(a.img, f"mm_latency_{tag}.png"))
    plt.close(fig)

    # ---------------- Figure 3: fill-model optimism ----------------
    fig, axs = plt.subplots(1, 2, figsize=(9.2, 3.8))
    q = g[(g.latency_ms == 10) & (g.fee_bps == 0) & (g.fill == "queue")].set_index("label").loc[order]
    o = g[(g.fill == "optimistic")].set_index("label").loc[order]
    x = np.arange(len(order))
    axs[0].bar(x - 0.2, q.fills, 0.4, color="#0072B2", label="queue-aware")
    axs[0].bar(x + 0.2, o.fills, 0.4, color="#E69F00", label="optimistic (touch = filled)")
    axs[0].set_title("number of fills")
    axs[1].bar(x - 0.2, q.total_pnl, 0.4, color="#0072B2")
    axs[1].bar(x + 0.2, o.total_pnl, 0.4, color="#E69F00")
    axs[1].axhline(0, color="black", lw=0.8)
    axs[1].set_title("total PnL (USDT)")
    for ax in axs:
        ax.set_xticks(x)
        ax.set_xticklabels(order, rotation=35, ha="right", fontsize=7)
    axs[0].legend(frameon=False, fontsize=8)
    fig.suptitle(f"{ds}: how much a naive fill model overstates results", y=1.02)
    fig.savefig(os.path.join(a.img, f"mm_fill_model_{tag}.png"))
    plt.close(fig)

    # ---------------- Figure 4: fees ----------------
    fig, ax = plt.subplots(figsize=(6.6, 4.2))
    for l in ["touch", ref_fixed] + [x for x in order if x.startswith("as")]:
        d = g[(g.label == l) & (g.latency_ms == 10) & (g.fill == "queue") & (g.fee_bps.isin([-0.5, 0, 1, 10]))].sort_values("fee_bps")
        ax.plot(d.fee_bps, d.total_pnl, marker="o", ms=4, color=COL[kind(l)], alpha=0.4 + 0.6 * (kind(l) == "as"), label=l)
    ax.set_xscale("symlog", linthresh=1)
    ax.set_xlabel("fee per fill, basis points of notional (negative = rebate)")
    ax.set_ylabel("total PnL (USDT)")
    ax.set_title(f"{ds}: fees dominate the economics")
    ax.legend(frameon=False, fontsize=7)
    fig.savefig(os.path.join(a.img, f"mm_fees_{tag}.png"))
    plt.close(fig)

    # ---------------- Figure 5: adverse selection ----------------
    fig, ax = plt.subplots(figsize=(6.6, 4.2))
    for l in order:
        r = main_.loc[l]
        ax.plot([0, 1, 5, 30], [r.edge_ticks, r.markout1 + r.edge_ticks * 0, r.markout5, r.markout30] if False else [0, r.markout1, r.markout5, r.markout30],
                marker="o", ms=4, color=COL[kind(l)], alpha=0.4 + 0.6 * (kind(l) == "as"), label=l)
    ax.axhline(0, color="black", lw=0.8)
    ax.set_xlabel("seconds after the fill")
    ax.set_ylabel("mid-price drift, signed toward our position (ticks)")
    ax.set_title(f"{ds}: adverse selection (negative = price moves against a fresh fill)")
    ax.legend(frameon=False, fontsize=7, ncol=2)
    fig.savefig(os.path.join(a.img, f"mm_adverse_selection_{tag}.png"))
    plt.close(fig)

    # ---------------- Figure 6: time series ----------------
    fig, axs = plt.subplots(2, 1, figsize=(8.6, 5.4), sharex=True)
    pick = ["touch", ref_fixed, as_labels[2]]  # the third gamma/k point: enough skew to matter, still fills
    for l in pick:
        s = pd.read_csv(run_series(a.results, ds, l))
        axs[0].plot(s.t_s / 60, s.pnl_usdt, color=COL[kind(l)], lw=1.2, label=l)
        axs[1].plot(s.t_s / 60, s.inv_btc, color=COL[kind(l)], lw=1.0)
    axs[0].set_ylabel("mark-to-market PnL (USDT)")
    axs[1].set_ylabel("inventory (base asset)")
    axs[1].set_xlabel("minutes")
    axs[0].legend(frameon=False, fontsize=8)
    axs[0].set_title(f"{ds}: PnL and inventory through the session")
    fig.savefig(os.path.join(a.img, f"mm_timeseries_{tag}.png"))
    plt.close(fig)

    # ---------------- the cancellation rule: proportional vs power 3 (L3 ground truth) ----------------
    if not alt_rule.empty:
        p3 = alt_rule.set_index("label").loc[order]
        rule = pd.DataFrame({"strategy": order, "fills_prop": main_.fills.to_numpy(), "fills_pow3": p3.fills.to_numpy(),
                             "total_prop": main_.total_pnl.to_numpy(), "total_pow3": p3.total_pnl.to_numpy(),
                             "bps_prop": (1e4 * main_.total_pnl / main_.notional).to_numpy(),
                             "bps_pow3": (1e4 * p3.total_pnl / p3.notional).to_numpy()})
        rule.to_csv(os.path.join(a.results, f"cancel_rule_{tag}.csv"), index=False)
        print("cancellation rule, proportional vs power 3:")
        print(rule.round(3).to_string(index=False))

    pd.set_option("display.width", 200, "display.max_columns", 30)
    print(tbl.round(3).to_string(index=False))
    print(json.dumps(diffs, indent=1))


if __name__ == "__main__":
    main()
