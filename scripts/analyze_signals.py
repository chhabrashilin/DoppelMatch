#!/usr/bin/env python3
"""Order-book signals on Coinbase BTC-USD, tested out of sample across days (input: exsim_features output).

    python analyze_signals.py results/signals --img docs/img --out results/signals/summary.json

Three questions, each answered day by day with held-out days as the test set:

1. Does queue imbalance at the best quotes predict the direction of the next mid-price move?
   (Gould and Bonart 2016, "Queue imbalance as a one-tick-ahead price predictor".) At the end of every
   one-second bar, I = (bid_qty - ask_qty) / (bid_qty + ask_qty); target: is the next mid move up?
   Logistic regression, leave one day out; scored by AUC and accuracy on the held-out day.

2. Is price impact linear in order-flow imbalance, contemporaneously? (Cont, Kukanov and Stoikov 2014.)
   Per day: regress the bar's mid change on the bar's OFI scaled by average best-level depth; report R^2.
   This is a replication, not a trading signal: both sides are measured over the same interval.

3. Do book features predict FUTURE returns, and is it enough to trade on?
   Features at time t (queue imbalance, top-5 depth imbalance, last-bar OFI, last-10 s OFI, last-bar trade
   imbalance), target: the mid return from t to t + h for h in {1, 10, 60} s, in basis points. OLS fitted on
   all other days, scored on the held-out day by out-of-sample R^2 against a zero forecast. One-sided
   test across days that OOS R^2 > 0, with Holm's correction over the whole grid (15 tests), so a result is
   not declared real just because one of fifteen tries looked good.

Bars that touch a gap in the archived feed are excluded (valid == 0), along with any target that spans one.
Intervals are bootstrap over days, the independent unit.
"""
import argparse, glob, json, math, os
import numpy as np
import pandas as pd

HORIZONS = [1, 10, 60]
FEATURES = ["qimb", "dimb5", "ofi1", "ofi10", "timb1"]


def load_day(prefix):
    b = pd.read_csv(prefix + ".bars.csv")
    m = pd.read_csv(prefix + ".moves.csv")
    b["mid2"] = b.bid + b.ask
    b["qimb"] = (b.bid_qty - b.ask_qty) / (b.bid_qty + b.ask_qty)
    b["dimb5"] = (b.bid5 - b.ask5) / (b.bid5 + b.ask5)
    depth = 0.5 * (b.bid_qty + b.ask_qty).mean()
    b["ofi1"] = b.ofi / depth                                   # CKS normalization by average depth
    b["ofi10"] = b.ofi.rolling(10, min_periods=10).sum() / depth
    tv = b.buy_vol + b.sell_vol
    b["timb1"] = np.where(tv > 0, (b.buy_vol - b.sell_vol) / tv.where(tv > 0, 1), 0.0)
    b["dmid_ticks"] = b.mid2.diff() / 2.0                         # this bar's mid change (for question 2)
    # validity: a bar, and every bar a target reaches into, must be clean
    bad = (b.valid == 0).astype(int).to_numpy()
    cbad = np.concatenate([[0], np.cumsum(bad)])
    idx = np.arange(len(b))
    for h in HORIZONS:
        j = np.minimum(idx + h, len(b) - 1)
        fut = b.mid2.to_numpy()[j]
        ok = (idx + h < len(b)) & (cbad[j + 1] - cbad[idx] == 0)
        b[f"ret{h}"] = np.where(ok, (fut - b.mid2) / b.mid2 * 1e4, np.nan)  # bps
    # next mid move after each bar end
    mt = m.t_ns.to_numpy()
    k = np.searchsorted(mt, b.t_ns.to_numpy(), side="right")
    has = k < len(mt)
    nxt = np.where(has, m.mid2.to_numpy()[np.minimum(k, len(mt) - 1)], np.nan)
    wait_s = np.where(has, (mt[np.minimum(k, len(mt) - 1)] - b.t_ns.to_numpy()) * 1e-9, np.nan)
    within = has & (wait_s <= 60)
    jmove = np.minimum(idx + np.ceil(np.nan_to_num(wait_s)).astype(int), len(b) - 1)
    clean = within & (cbad[jmove + 1] - cbad[idx] == 0)
    b["up"] = np.where(clean & (nxt != b.mid2), (nxt > b.mid2).astype(float), np.nan)
    b = b[(b.valid == 1) & b.qimb.notna()]
    return b


def auc(score, y):
    order = np.argsort(score, kind="mergesort")
    ranks = np.empty(len(score))
    ranks[order] = np.arange(1, len(score) + 1)
    # average ranks for ties
    s = score[order]
    i = 0
    while i < len(s):
        j = i
        while j + 1 < len(s) and s[j + 1] == s[i]:
            j += 1
        if j > i:
            ranks[order[i:j + 1]] = (i + j + 2) / 2.0
        i = j + 1
    n1 = y.sum()
    n0 = len(y) - n1
    return (ranks[y == 1].sum() - n1 * (n1 + 1) / 2) / (n1 * n0)


def logit_fit(X, y, iters=25):
    X = np.column_stack([np.ones(len(X)), X])
    w = np.zeros(X.shape[1])
    for _ in range(iters):
        p = 1 / (1 + np.exp(-X @ w))
        g = X.T @ (y - p)
        H = (X * (p * (1 - p))[:, None]).T @ X + 1e-9 * np.eye(X.shape[1])
        w += np.linalg.solve(H, g)
    return w


def logit_predict(w, X):
    X = np.column_stack([np.ones(len(X)), X])
    return 1 / (1 + np.exp(-X @ w))


def day_bootstrap_ci(values, n=10000, seed=1):
    v = np.asarray(values, dtype=float)
    rng = np.random.default_rng(seed)
    means = rng.choice(v, size=(n, len(v)), replace=True).mean(axis=1)
    return float(v.mean()), float(np.quantile(means, 0.025)), float(np.quantile(means, 0.975))


def one_sided_p(values):
    """t-test that the mean is > 0 (days as observations); Student t via the regularized incomplete beta."""
    v = np.asarray(values, dtype=float)
    n = len(v)
    if n < 2 or v.std(ddof=1) == 0:
        return float("nan")
    t = v.mean() / (v.std(ddof=1) / math.sqrt(n))
    df = n - 1
    x = df / (df + t * t)
    p_two = betainc(df / 2, 0.5, x)
    return p_two / 2 if t > 0 else 1 - p_two / 2


def _betacf(a, b, x, maxit=300, eps=3e-14, tiny=1e-300):
    """Continued fraction for the incomplete beta function (Numerical Recipes, 2nd ed., betacf)."""
    qab, qap, qam = a + b, a + 1, a - 1
    c, d = 1.0, 1 - qab * x / qap
    d = 1 / (d if abs(d) > tiny else tiny)
    h = d
    for m in range(1, maxit + 1):
        m2 = 2 * m
        for aa in (m * (b - m) * x / ((qam + m2) * (a + m2)), -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))):
            d = 1 + aa * d
            d = 1 / (d if abs(d) > tiny else tiny)
            c = 1 + aa / c
            c = c if abs(c) > tiny else tiny
            h *= d * c
        if abs(d * c - 1) < eps:
            break
    return h


def betainc(a, b, x):
    """Regularized incomplete beta I_x(a, b)."""
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0
    bt = math.exp(math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) + a * math.log(x) + b * math.log(1 - x))
    if x < (a + 1) / (a + b + 2):
        return bt * _betacf(a, b, x) / a
    return 1 - bt * _betacf(b, a, 1 - x) / b


def holm(pvals):
    items = sorted((p, k) for k, p in pvals.items() if not math.isnan(p))
    m, out, running = len(items), {}, 0.0
    for i, (p, k) in enumerate(items):
        running = max(running, min(1.0, (m - i) * p))
        out[k] = running
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--img")
    ap.add_argument("--out")
    a = ap.parse_args()
    prefixes = sorted(p[: -len(".bars.csv")] for p in glob.glob(os.path.join(a.dir, "*.bars.csv")))
    days = {os.path.basename(p): load_day(p) for p in prefixes}
    names = sorted(days)
    print(f"{len(names)} days: {', '.join(names)}")
    print(f"bars (valid): {sum(len(d) for d in days.values()):,}")
    summary = {"days": names}

    # ---- 1. queue imbalance -> next mid move ----
    print("\n1. queue imbalance at the best quotes -> direction of the next mid move (leave one day out)")
    q1 = {}
    for feat in ["qimb", "dimb5"]:
        aucs, accs, base = [], [], []
        for test in names:
            tr = pd.concat([days[d] for d in names if d != test])
            tr = tr[tr.up.notna()]
            te = days[test][days[test].up.notna()]
            w = logit_fit(tr[[feat]].to_numpy(), tr.up.to_numpy())
            p = logit_predict(w, te[[feat]].to_numpy())
            y = te.up.to_numpy()
            aucs.append(auc(p, y))
            accs.append(((p > 0.5) == (y == 1)).mean())
            base.append(max(y.mean(), 1 - y.mean()))
        m, lo, hi = day_bootstrap_ci(aucs)
        am, alo, ahi = day_bootstrap_ci(accs)
        print(f"  {feat:6s} AUC {m:.3f} [{lo:.3f}, {hi:.3f}]   accuracy {am:.3f} [{alo:.3f}, {ahi:.3f}]"
              f"   (majority-class baseline {np.mean(base):.3f}); worst day AUC {min(aucs):.3f}")
        q1[feat] = {"auc": [m, lo, hi], "accuracy": [am, alo, ahi], "baseline": float(np.mean(base)),
                    "auc_by_day": dict(zip(names, map(float, aucs)))}
    # calibration table, pooled
    pooled = pd.concat(days.values())
    pooled = pooled[pooled.up.notna()]
    bins = np.linspace(-1, 1, 11)
    pooled["bin"] = pd.cut(pooled.qimb, bins, include_lowest=True)
    cal = pooled.groupby("bin", observed=True).up.agg(["mean", "count"])
    print("  P(next move up | queue imbalance), pooled:")
    for iv, row in cal.iterrows():
        print(f"    {str(iv):>16s}  {row['mean']:.3f}  (n={int(row['count']):,})")
    q1["calibration"] = [[float(iv.mid), float(r["mean"]), int(r["count"])] for iv, r in cal.iterrows()]
    summary["queue_imbalance"] = q1

    # ---- 2. contemporaneous OFI -> mid change ----
    print("\n2. contemporaneous price impact of order-flow imbalance (Cont, Kukanov and Stoikov 2014)")
    q2 = {}
    for agg in [1, 10, 60]:
        r2s, betas = [], []
        for d in names:
            b = days[d]
            g = (np.arange(len(b)) // agg)
            x = b.ofi1.groupby(g).sum().to_numpy()
            y = b.dmid_ticks.groupby(g).sum().to_numpy()
            ok = np.isfinite(x) & np.isfinite(y)
            x, y = x[ok], y[ok]
            beta = (x @ y) / (x @ x)
            r2s.append(1 - ((y - beta * x) ** 2).sum() / ((y - y.mean()) ** 2).sum())
            betas.append(beta)
        m, lo, hi = day_bootstrap_ci(r2s)
        print(f"  {agg:3d} s intervals: R^2 {m:.3f} [{lo:.3f}, {hi:.3f}], range over days {min(r2s):.3f}-{max(r2s):.3f}")
        q2[f"{agg}s"] = {"r2": [m, lo, hi], "r2_by_day": dict(zip(names, map(float, r2s)))}
    summary["ofi_contemporaneous"] = q2

    # ---- 3. predicting future returns ----
    print("\n3. predicting the mid return over the next h seconds (OLS, leave one day out; OOS R^2 vs zero forecast)")
    pvals, table = {}, {}
    for h in HORIZONS:
        for feat in FEATURES:
            r2s = []
            for test in names:
                tr = pd.concat([days[d] for d in names if d != test])[[feat, f"ret{h}"]].dropna()
                te = days[test][[feat, f"ret{h}"]].dropna()
                X = np.column_stack([np.ones(len(tr)), tr[feat].to_numpy()])
                w = np.linalg.lstsq(X, tr[f"ret{h}"].to_numpy(), rcond=None)[0]
                pred = w[0] + w[1] * te[feat].to_numpy()
                y = te[f"ret{h}"].to_numpy()
                r2s.append(1 - ((y - pred) ** 2).sum() / (y ** 2).sum())
            key = f"{feat}@{h}s"
            pvals[key] = one_sided_p(r2s)
            table[key] = {"oos_r2": day_bootstrap_ci(r2s), "days_positive": int(sum(r > 0 for r in r2s)),
                          "slope_bps_per_unit": float(w[1])}
    adj = holm(pvals)
    print(f"  {'feature@h':12s} {'OOS R^2 (mean [95% CI])':>30s} {'days > 0':>9s} {'p':>9s} {'Holm p':>9s}")
    for key, t in table.items():
        m, lo, hi = t["oos_r2"]
        t["p"], t["holm_p"] = pvals[key], adj.get(key, float("nan"))
        mark = " *" if t["holm_p"] < 0.05 else ""
        print(f"  {key:12s} {m:10.4f} [{lo:8.4f}, {hi:8.4f}] {t['days_positive']:5d}/{len(names):<3d} "
              f"{t['p']:9.2g} {t['holm_p']:9.2g}{mark}")
    summary["predictive"] = table

    # economic scale: typical predicted move vs costs
    b = pooled
    spread_bps = float(((b.ask - b.bid) / (b.mid2 / 2) * 1e4).median())
    sd = {h: float(pd.concat(days.values())[f"ret{h}"].std()) for h in HORIZONS}
    print(f"\n  median spread {spread_bps:.4f} bps; std of mid return: " +
          ", ".join(f"{h} s {sd[h]:.2f} bps" for h in HORIZONS))
    summary["scale"] = {"median_spread_bps": spread_bps, "ret_std_bps": sd}

    if a.img:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(1, 2, figsize=(11, 4))
        for d in names:
            dd = days[d][days[d].up.notna()].copy()
            dd["bin"] = pd.cut(dd.qimb, bins, include_lowest=True)
            c = dd.groupby("bin", observed=True).up.mean()
            ax[0].plot([iv.mid for iv in c.index], c.values, color="0.6", lw=0.8)
        ax[0].plot([r[0] for r in q1["calibration"]], [r[1] for r in q1["calibration"]], color="C0", lw=2.5, label="pooled")
        ax[0].axhline(0.5, color="k", lw=0.5)
        ax[0].set_xlabel("queue imbalance at the best quotes")
        ax[0].set_ylabel("P(next mid move is up)")
        ax[0].set_title("Queue imbalance vs next move (grey: one line per day)")
        ax[0].legend()
        keys = list(table)
        ms = [table[k]["oos_r2"][0] for k in keys]
        err = [[table[k]["oos_r2"][0] - table[k]["oos_r2"][1] for k in keys],
               [table[k]["oos_r2"][2] - table[k]["oos_r2"][0] for k in keys]]
        colors = ["C2" if table[k]["holm_p"] < 0.05 else "C7" for k in keys]
        ax[1].barh(range(len(keys)), ms, xerr=err, color=colors)
        ax[1].set_yticks(range(len(keys)))
        ax[1].set_yticklabels(keys, fontsize=8)
        ax[1].axvline(0, color="k", lw=0.5)
        ax[1].set_xlabel("out-of-sample R^2 (held-out days; green: Holm p < 0.05)")
        ax[1].set_title("Predicting the next h seconds")
        fig.tight_layout()
        os.makedirs(a.img, exist_ok=True)
        fig.savefig(os.path.join(a.img, "signals_coinbase.png"), dpi=130)
        print(f"\nfigure: {os.path.join(a.img, 'signals_coinbase.png')}")
    if a.out:
        with open(a.out, "w") as f:
            json.dump(summary, f, indent=1, default=float)


if __name__ == "__main__":
    main()
