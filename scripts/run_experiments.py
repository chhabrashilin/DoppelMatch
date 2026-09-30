#!/usr/bin/env python3
"""Runs the full market-making experiment grid and writes results/grid.csv (+ per-run series for plots).

    python run_experiments.py --bin build/release/exsim_mmsim \
        --data btcusdt_a.exmd:0.00243 ethusdt_b.exmd:0.0031 --out results

Each --data entry is  file:k  where k is that market's Avellaneda-Stoikov decay parameter estimated by
calibrate_k.py from that same capture. Every cell is an independent, deterministic run of
`exsim_mmsim --json`; the grid is fully specified below, so every number in the write-up traces to a row.
"""
import argparse, csv, json, os, subprocess
from concurrent.futures import ThreadPoolExecutor

# The Avellaneda-Stoikov risk aversion is swept in the dimensionless ratio gamma/k, fixed in advance (not
# tuned on the data), so two markets with very different tick sizes get the same principled grid.
GAMMA_OVER_K = [4e-4, 4e-3, 4e-2, 4e-1]
LATENCIES = [0, 10, 50, 200]
FEES = [10, 1, -0.5]


def cell(binary, data, k, extra, prefix=None):
    cmd = [binary, "--in", data, "--json", "--k", str(k)] + [str(x) for x in extra]
    if prefix:
        cmd += ["--out-prefix", prefix]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode not in (0, 3):
        raise RuntimeError(f"{' '.join(cmd)}\n{out.stderr}")
    row = json.loads(out.stdout.strip().splitlines()[-1])
    row["dataset"] = os.path.basename(data).split(".")[0]
    row["label"] = label(extra)
    return row


def label(extra):
    d = dict(zip(extra[::2], extra[1::2]))
    s = d.get("--strategy", "as")
    if s == "as":
        return f"as g={float(d.get('--gamma', 0.1)):g}"
    if s == "fixed":
        return f"fixed h={d.get('--half-spread', 2)}"
    return "touch"


def strategies(k):
    """touch, fixed spreads (including one as wide as A-S's own quotes, 1/k ticks: the fair control), A-S."""
    yield [("--strategy", "touch")]
    for h in sorted({2, 10, 100, round(1 / k)}):
        yield [("--strategy", "fixed"), ("--half-spread", h)]
    for r in GAMMA_OVER_K:
        yield [("--strategy", "as"), ("--gamma", float(f"{r * k:.3g}"))]


def flat(pairs):
    return [x for p in pairs for x in p]


def build_cells(datasets, out_dir):
    cells = []
    for path, k in datasets:
        tag = os.path.basename(path).split(".")[0]
        for strat in strategies(k):
            base = flat(strat)
            # main comparison: 10 ms latency, no fee; keep series and fills for plots and the bootstrap
            cells.append((path, k, base + ["--latency-ms", 10, "--fee-bps", 0], os.path.join(out_dir, "runs", f"{tag}_{label(base).replace(' ', '_').replace('=', '')}")))
            for lat in LATENCIES:
                if lat != 10:
                    cells.append((path, k, base + ["--latency-ms", lat, "--fee-bps", 0], None))
            cells.append((path, k, base + ["--latency-ms", 10, "--fill", "optimistic"], None))
            # the cancellation rule that Coinbase L3 ground truth supports (RESEARCH.md part 2), vs proportional
            cells.append((path, k, base + ["--latency-ms", 10, "--fee-bps", 0, "--cancel-power", 3], None))
            for fee in FEES:
                cells.append((path, k, base + ["--latency-ms", 10, "--fee-bps", fee], None))
    return cells


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--data", nargs="+", required=True, help="file:k pairs")
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()
    datasets = []
    for d in a.data:
        p, k = d.rsplit(":", 1)
        datasets.append((p, float(k)))
    os.makedirs(os.path.join(a.out, "runs"), exist_ok=True)
    cells = build_cells(datasets, a.out)
    print(f"{len(cells)} runs")
    with ThreadPoolExecutor(a.jobs) as ex:
        rows = list(ex.map(lambda c: cell(a.bin, c[0], c[1], c[2], c[3]), cells))
    path = os.path.join(a.out, "grid.csv")
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print("wrote", path)


if __name__ == "__main__":
    main()
