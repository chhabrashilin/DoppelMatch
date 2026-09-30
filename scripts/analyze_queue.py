#!/usr/bin/env python3
"""Scores L2 queue-position rules against L3 ground truth (output of exsim_queuestudy), pooled over days.

    python analyze_queue.py results/queue/*.probes.csv --img docs/img --out results/queue/summary.json

Only fills that depend on queue position are informative. If the price trades THROUGH a probe's level, every
rule fills it at the same moment by construction, so the evaluation is restricted to probes whose true fill
(if any) happened by a trade AT their price, and to queues that had more than one order when the probe
joined. For those, each rule's predicted fill is compared with the truth:
  * fill classification within the horizon (precision, recall, and the fill-rate ratio predicted / true);
  * timing error for probes both filled: predicted minus true, in seconds (negative = predicted too early,
    i.e. an optimistic backtest).
Intervals are bootstrap over DAYS (the independent unit), not over probes.
"""
import argparse, json, os, re
import numpy as np
import pandas as pd

RULES = ["front", "prop", "pow2", "pow3", "log", "back"]


def score(df):
    out = {}
    tf = df.true_fill_s >= 0
    for r in RULES:
        ef = df[f"{r}_fill_s"] >= 0
        tp = (tf & ef).sum()
        both = df[tf & ef]
        err = both[f"{r}_fill_s"] - both.true_fill_s
        out[r] = {
            "precision": tp / max(1, ef.sum()),
            "recall": tp / max(1, tf.sum()),
            "fill_ratio": ef.sum() / max(1, tf.sum()),
            "median_err_s": float(err.median()) if len(err) else np.nan,
            "mean_abs_err_s": float(err.abs().mean()) if len(err) else np.nan,
            "early_share": float((err < -0.001).mean()) if len(err) else np.nan,
            "late_share": float((err > 0.001).mean()) if len(err) else np.nan,
        }
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--img")
    ap.add_argument("--out")
    ap.add_argument("--min-orders", type=int, default=2)
    a = ap.parse_args()
    frames = []
    for path in a.csv:
        d = pd.read_csv(path)
        d["day"] = os.path.basename(path).split(".")[0]
        frames.append(d)
    df = pd.concat(frames, ignore_index=True)
    n_all = len(df)
    sel = df[(df.fill_kind != 2) & (df.n0 >= a.min_orders)].copy()  # queue-sensitive probes
    print(f"{n_all} probes over {df.day.nunique()} day(s); queue-sensitive (no trade-through, >= {a.min_orders} orders at join): {len(sel)}")
    print(f"  of those, truly filled at their price within the horizon: {(sel.true_fill_s >= 0).mean():.1%}")
    pooled = score(sel)
    # bootstrap over days
    days = sel.day.unique()
    rng = np.random.default_rng(7)
    boots = {r: {"fill_ratio": [], "median_err_s": [], "early_share": [], "late_share": [], "mean_abs_err_s": []} for r in RULES}
    if len(days) > 1:
        by_day = {d: sel[sel.day == d] for d in days}
        for _ in range(2000):
            pick = rng.choice(days, size=len(days), replace=True)
            s = score(pd.concat([by_day[d] for d in pick]))
            for r in RULES:
                for k in boots[r]:
                    boots[r][k].append(s[r][k])
    print(f"\n{'rule':6} {'precision':>9} {'recall':>7} {'fills pred/true':>16} {'median err s':>13} {'mean |err| s':>13} {'early':>6} {'late':>6}")
    for r in RULES:
        s = pooled[r]
        ci = ""
        if boots[r]["fill_ratio"]:
            lo, hi = np.percentile(boots[r]["fill_ratio"], [2.5, 97.5])
            ci = f" [{lo:.2f},{hi:.2f}]"
        print(f"{r:6} {s['precision']:9.3f} {s['recall']:7.3f} {s['fill_ratio']:8.3f}{ci:>8} {s['median_err_s']:13.2f} "
              f"{s['mean_abs_err_s']:13.2f} {s['early_share']:6.1%} {s['late_share']:6.1%}")
    cis = {r: {k: list(np.percentile(v, [2.5, 97.5])) for k, v in boots[r].items() if v} for r in RULES}
    if cis[RULES[0]]:
        print("\n95% day-bootstrap intervals: early share / mean |err| s")
        for r in RULES:
            e, m = cis[r]["early_share"], cis[r]["mean_abs_err_s"]
            print(f"  {r:6} early [{e[0]:.1%}, {e[1]:.1%}]   mean |err| [{m[0]:.2f}, {m[1]:.2f}]")

    # Where cancellations come from: exsim_queuestudy's per-day report (<prefix>.txt) has the exact normalized rank.
    cancel = {}
    pat = re.compile(r"^\s+(2-4|5-9|10\+) orders\s+mean ([0-9.]+), front half ([0-9.]+)% vs back half ([0-9.]+)%")
    for path in a.csv:
        report = path[: -len(".probes.csv")] + ".txt"
        if not os.path.exists(report):
            continue
        for line in open(report):
            m = pat.match(line)
            if m:
                cancel.setdefault(m.group(1), []).append((float(m.group(2)), float(m.group(4)) / 100))
    if cancel:
        print("\nnormalized rank of cancelled orders at the touch (0 front, 1 back; 0.5 = uniform), mean over days:")
        for b in ["2-4", "5-9", "10+"]:
            v = np.array(cancel.get(b, []))
            if len(v):
                bs = [v[rng.integers(0, len(v), len(v))].mean(axis=0) for _ in range(2000)]
                lo, hi = np.percentile(bs, [2.5, 97.5], axis=0)
                print(f"  {b:>4} orders: mean rank {v[:, 0].mean():.3f} [{lo[0]:.3f}, {hi[0]:.3f}], back half {v[:, 1].mean():.1%} "
                      f"[{lo[1]:.1%}, {hi[1]:.1%}], days {len(v)}, range {v[:, 0].min():.3f}-{v[:, 0].max():.3f}")
    if a.out:
        with open(a.out, "w") as f:
            json.dump({"probes": n_all, "queue_sensitive": len(sel), "days": list(map(str, days)), "rules": pooled,
                       "bootstrap_ci": cis,
                       "cancel_rank_by_day": {b: [list(x) for x in v] for b, v in cancel.items()}},
                      f, indent=2, default=float)
    if a.img:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import matplotlib.ticker
        # One measure per panel, one hue, 95% day-bootstrap intervals; values labelled on the bars.
        ink, muted, hue = "#0b0b0b", "#52514e", "#2a78d6"
        panels = [("early_share", "predicted fill too early", "share of filled probes", True),
                  ("late_share", "predicted fill too late", "share of filled probes", True),
                  ("mean_abs_err_s", "mean timing error", "seconds", False)]
        fig, axs = plt.subplots(1, 3, figsize=(12, 3.6))
        x = np.arange(len(RULES))
        for ax, (key, title, ylabel, pct) in zip(axs, panels):
            v = np.array([pooled[r][key] for r in RULES], dtype=float)
            ax.bar(x, v, width=0.6, color=hue, edgecolor="white", linewidth=2)
            top = v.copy()
            if key in cis[RULES[0]]:
                lo = np.array([cis[r][key][0] for r in RULES])
                hi = np.array([cis[r][key][1] for r in RULES])
                ax.errorbar(x, v, yerr=[np.maximum(v - lo, 0), np.maximum(hi - v, 0)], fmt="none", ecolor=ink, lw=1, capsize=3)
                top = np.maximum(v, hi)
            pad = 0.02 * top.max()
            for xi, vi, ti in zip(x, v, top):
                ax.text(xi, ti + pad, f"{vi:.0%}" if pct else f"{vi:.1f}", ha="center", va="bottom", fontsize=8, color=ink)
            ax.set_ylim(0, top.max() * 1.15)
            ax.set_xticks(x, RULES, color=ink)
            ax.set_title(title, color=ink, fontsize=11)
            ax.set_ylabel(ylabel, color=muted)
            if pct:
                ax.yaxis.set_major_formatter(matplotlib.ticker.PercentFormatter(1.0, decimals=0))
            for s in ("top", "right"):
                ax.spines[s].set_visible(False)
            ax.grid(axis="y", color="#e6e5e1", lw=0.8)
            ax.set_axisbelow(True)
        fig.suptitle(f"Six level-2 queue models scored against Coinbase level-3 truth ({len(days)} days, "
                     f"{len(sel):,} queue-sensitive probes; bars: 95% day-bootstrap intervals)", fontsize=10, color=muted)
        fig.tight_layout()
        os.makedirs(a.img, exist_ok=True)
        fig.savefig(os.path.join(a.img, "queue_rules_vs_truth.png"), dpi=140)


if __name__ == "__main__":
    main()
