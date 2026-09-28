#!/usr/bin/env python3
"""Collects exsim_l3replay reports (one per day) into the validation table in docs/VALIDATION.md.

    python summarize_l3.py results/l3/*.txt --out results/l3/summary.json

Prints a Markdown table (one row per day plus a total) and writes the parsed numbers as JSON.
"""
import argparse, json, os, re

FIELDS = {
    "records": r"^records (\d+)",
    "gaps": r"sequence gaps (\d+)",
    "resyncs": r"recovered from a later snapshot: (\d+)",
    "truth_errors": r"truth-book inconsistencies (\d+)",
    "received": r"^orders received (\d+)",
    "cancels": r"cancels (\d+), modifies",
    "modifies": r"modifies (\d+) \(",
    "episodes": r"^episodes \(arrivals \+ price/size modifies\): (\d+)",
    "exact": r"reproduced exactly .*?: (\d+) \(",
    "diverged": r"diverged: (\d+)",
    "diverged_stp": r"diverged: \d+ \((\d+) involving",
    "matches": r"^Coinbase matches: (\d+)",
    "matches_exact": r"predicted exactly by the engine: (\d+)",
    "rest_checked": r"checked against Coinbase's 'open': (\d+)",
    "rest_mismatch": r"'open': \d+, mismatched (\d+)",
    "book_checks": r"every order in queue order\): (\d+)",
    "book_failed": r"queue order\): \d+, failed (\d+)",
    "unmatched_fill": r"exchange inconsistency\): (\d+) orders",
    "stp_links": r"self-trade prevention: (\d+) links",
    "cn_links": r"\((\d+) of them from cancel-newest",
    "cb_orders": r"(\d+) arrivals cancelled together with their own resting order",
    "unmatched_fill_qty": r"exchange inconsistency\): \d+ orders, (\d+) lots",
    "funds_orders": r"funds-denominated (\d+)\)",
    "ioc_inferred": r"inferred from lifecycle: (\d+) IOC/FOK",
    "post_only_inferred": r"(\d+) post-only/zero-fill",
    "stp_orders": r"(\d+) arrivals with a self-trade-prevention mode",
    "stp_changes": r"STP changes (\d+)",
    "clamped": r"band: (\d+) arrivals or fill-modifies clamped",
    "out_of_band": r"(\d+) rest out of band",
}


def parse(path):
    text = open(path).read()
    row = {"day": os.path.basename(path).split(".")[0]}
    for k, pat in FIELDS.items():
        m = re.search(pat, text, re.M)
        row[k] = int(m.group(1)) if m else 0
    row["in_sync_at_end"] = "ENDED OUT OF SYNC" not in text
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reports", nargs="+")
    ap.add_argument("--out")
    a = ap.parse_args()
    rows = sorted((parse(p) for p in a.reports), key=lambda r: r["day"])
    total = {k: sum(r[k] for r in rows) for k in FIELDS}
    total["day"] = f"**all {len(rows)} days**"
    pct = lambda a, b: f"{100 * a / b:.4f}%" if b else "-"
    print("| day | messages | feed gaps (recovered) | episodes | reproduced exactly | diverged (STP) | "
          "Coinbase trades | predicted exactly | full-book checks (failed) |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in rows + [total]:
        print(f"| {r['day']} | {r['records']:,} | {r['gaps']} ({r['resyncs']}) | {r['episodes']:,} | "
              f"{r['exact']:,} ({pct(r['exact'], r['episodes'])}) | {r['diverged']} ({r['diverged_stp']}) | "
              f"{r['matches']:,} | {r['matches_exact']:,} ({pct(r['matches_exact'], r['matches'])}) | "
              f"{r['book_checks']} ({r['book_failed']}) |")
    print(f"\nall days back in sync at the end: {all(r['in_sync_at_end'] for r in rows)}")
    print(f"truth-book inconsistencies: {total['truth_errors']}; resting remainders checked {total['rest_checked']:,}, "
          f"mismatched {total['rest_mismatch']}")
    print(f"orders reported filled with part never matched: {total['unmatched_fill']} ({total['unmatched_fill_qty']:,} lots)")
    print(f"self-trade prevention: {total['stp_links']:,} account links inferred ({total['cn_links']:,} cancel-newest), "
          f"{total['cb_orders']:,} cancel-both arrivals, {total['stp_orders']:,} arrivals given a mode, "
          f"{total['stp_changes']:,} STP decrements")
    print(f"inferred inputs: {total['ioc_inferred']:,} IOC/FOK, {total['post_only_inferred']:,} post-only, "
          f"{total['funds_orders']:,} funds-denominated sizes; clamped {total['clamped']:,}, "
          f"resting beyond the band (not replayable) {total['out_of_band']:,}")
    print(f"received {total['received']:,}, cancels {total['cancels']:,}, modifies {total['modifies']:,}")
    if a.out:
        with open(a.out, "w") as f:
            json.dump({"days": rows, "total": total}, f, indent=1)


if __name__ == "__main__":
    main()
