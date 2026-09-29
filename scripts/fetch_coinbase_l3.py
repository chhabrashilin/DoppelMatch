#!/usr/bin/env python3
"""Download historical Coinbase order-by-order (L3, "full" channel) data from Tardis.dev.

Tardis serves the first day of every month without an API key. The full channel gives every order
event on the exchange: received, open, done, match and change, each with its order id. Tardis also emits a
generated L3 snapshot (`full_snapshot`) when its capture connection starts, which is at 00:00 UTC. That
snapshot is the only way to seed the book, so a usable capture always starts at 00:00 and is contiguous.

    python fetch_coinbase_l3.py --date 2026-09-01 --hours 24 --out data/l3/2026-09-01
    python fetch_coinbase_l3.py --date 2026-09-01 --minutes 5 --out /tmp/l3    (a small sample, as CI does)

The API returns one minute per request. Minutes are fetched in parallel and written as one gzip file per
hour (raw Tardis lines: "<local ts> <json message>"). Hours already present are skipped, so an interrupted
download resumes. Set TARDIS_API_KEY to fetch days other than the first of a month.
"""
import argparse, datetime as dt, gzip, json, os, sys, time, urllib.parse, urllib.request
from concurrent.futures import ThreadPoolExecutor

CHANNELS = ["received", "open", "done", "change", "match", "full_snapshot"]


def fetch_minute(symbol, t, key=None):
    filters = json.dumps([{"channel": c, "symbols": [symbol]} for c in CHANNELS])
    q = urllib.parse.urlencode({"from": t.strftime("%Y-%m-%dT%H:%M:00.000Z"),
                                "to": (t + dt.timedelta(minutes=1)).strftime("%Y-%m-%dT%H:%M:00.000Z"),
                                "filters": filters})
    req = urllib.request.Request(f"https://api.tardis.dev/v1/data-feeds/coinbase?{q}",
                                 headers={"Accept-Encoding": "gzip", "User-Agent": "doppelmatch"})
    if key:
        req.add_header("Authorization", f"Bearer {key}")
    for attempt in range(8):
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                body = r.read()
                if r.headers.get("Content-Encoding") == "gzip":
                    body = gzip.decompress(body)
                return body
        except Exception as e:  # rate limit or transient network error: back off and retry
            print(f"  {t:%H:%M} retry {attempt + 1}: {e}", file=sys.stderr, flush=True)
            time.sleep(3 * (attempt + 1))
    raise RuntimeError(f"failed to download minute {t}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", required=True, help="YYYY-MM-DD (free without a key: the 1st of a month)")
    ap.add_argument("--hours", type=int, default=24, help="hours from 00:00 UTC")
    ap.add_argument("--minutes", type=int, default=0, help="only the first N minutes from 00:00 (a small sample)")
    ap.add_argument("--symbol", default="BTC-USD")
    ap.add_argument("--out", required=True)
    ap.add_argument("--threads", type=int, default=8)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    day = dt.datetime.fromisoformat(f"{a.date}T00:00:00")
    key = os.environ.get("TARDIS_API_KEY")
    with ThreadPoolExecutor(a.threads) as ex:
        per_hour = a.minutes if a.minutes else 60
        for h in range(1 if a.minutes else a.hours):
            t0 = day + dt.timedelta(hours=h)
            path = os.path.join(a.out, f"{a.symbol}_{t0:%Y%m%dT%H}.txt.gz")
            if os.path.exists(path):
                continue
            s = time.time()
            minutes = list(ex.map(lambda m: fetch_minute(a.symbol, t0 + dt.timedelta(minutes=m), key), range(per_hour)))
            tmp = path + ".part"
            with gzip.open(tmp, "wb", compresslevel=3) as f:
                for body in minutes:
                    f.write(body)
                    if body and not body.endswith(b"\n"):  # never let two minutes glue into one line
                        f.write(b"\n")
            os.replace(tmp, path)
            mb = sum(len(b) for b in minutes) / 1e6
            print(f"{os.path.basename(path)}: {mb:.0f} MB raw, {os.path.getsize(path) / 1e6:.0f} MB gz, {time.time() - s:.0f} s", flush=True)


if __name__ == "__main__":
    main()
