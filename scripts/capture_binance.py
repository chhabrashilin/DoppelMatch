#!/usr/bin/env python3
"""Record live Binance spot market data to a JSONL file.

Three streams go into one file, each line stamped with the local receive time `rx` (ns, monotonic
wall clock). Using ONE clock for everything matters: mixing exchange timestamps for trades with
local timestamps for book updates skews every fill-relative measurement (the exchange and this host
disagree by seconds).

  diff   {"k":"diff","rx":..,"U":..,"u":..,"b":[[px,qty],..],"a":[..]}   @depth@100ms
  trade  {"k":"trade","rx":..,"p":"..","q":"..","m":bool}                @trade
  snap   {"k":"snap","rx":..,"id":lastUpdateId,"b":[..],"a":[..]}        REST /depth, every --snap-every s

Follows Binance's documented local-book procedure: open the websocket FIRST, then fetch snapshots, so
no diff can fall between them. Uses public market-data endpoints; no API key.

    python capture_binance.py --minutes 30 --out data/btcusdt.jsonl
"""
import argparse, asyncio, json, os, sys, time, urllib.request

try:
    import websockets
except ImportError:
    sys.exit("pip install websockets")

WS = "wss://data-stream.binance.vision/stream?streams={s}@depth@100ms/{s}@trade"
REST = "https://data-api.binance.vision/api/v3/depth?symbol={S}&limit={n}"


def fetch_snapshot(symbol: str, limit: int) -> dict:
    req = urllib.request.Request(REST.format(S=symbol.upper(), n=limit), headers={"User-Agent": "doppelmatch"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())


async def snapshots(out, symbol, every, limit, stop):
    loop = asyncio.get_running_loop()
    while not stop.is_set():
        try:
            rx = time.time_ns()
            snap = await loop.run_in_executor(None, fetch_snapshot, symbol, limit)
            rx_done = time.time_ns()
            # rx is the time the response was received; the snapshot is as of some moment before that.
            out.write(json.dumps({"k": "snap", "rx": rx_done, "rx_req": rx, "id": snap["lastUpdateId"],
                                  "b": snap["bids"], "a": snap["asks"]}, separators=(",", ":")) + "\n")
        except Exception as e:  # network hiccup: skip this snapshot
            print("snapshot failed:", e, file=sys.stderr)
        try:
            await asyncio.wait_for(stop.wait(), every)
        except asyncio.TimeoutError:
            pass


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbol", default="BTCUSDT")
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--out", default="data/btcusdt.jsonl")
    ap.add_argument("--snap-every", type=float, default=2.0, help="seconds between REST snapshots")
    ap.add_argument("--snap-limit", type=int, default=1000)
    a = ap.parse_args()
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    sym = a.symbol.lower()
    stop = asyncio.Event()
    counts = {"diff": 0, "trade": 0, "snap": 0}
    with open(a.out, "w", buffering=1 << 20) as out:
        async with websockets.connect(WS.format(s=sym), max_size=None, ping_interval=20) as ws:
            # first snapshot only after the socket is open
            snap_task = asyncio.create_task(snapshots(out, a.symbol, a.snap_every, a.snap_limit, stop))
            deadline = time.time() + a.minutes * 60
            last_report = time.time()
            while time.time() < deadline:
                raw = await asyncio.wait_for(ws.recv(), 30)
                rx = time.time_ns()
                m = json.loads(raw)["data"]
                if m["e"] == "depthUpdate":
                    out.write(json.dumps({"k": "diff", "rx": rx, "U": m["U"], "u": m["u"], "b": m["b"], "a": m["a"]},
                                         separators=(",", ":")) + "\n")
                    counts["diff"] += 1
                elif m["e"] == "trade":
                    out.write(json.dumps({"k": "trade", "rx": rx, "p": m["p"], "q": m["q"], "m": m["m"]},
                                         separators=(",", ":")) + "\n")
                    counts["trade"] += 1
                if time.time() - last_report > 60:
                    last_report = time.time()
                    print(f"[{a.symbol}] {counts['diff']} diffs, {counts['trade']} trades, "
                          f"{int(deadline - time.time())}s left", flush=True)
            stop.set()
            await snap_task
    print("done:", counts, "->", a.out)


if __name__ == "__main__":
    asyncio.run(main())
