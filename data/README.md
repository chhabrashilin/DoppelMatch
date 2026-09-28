# Market data

`sample_btcusdt_30s.exmd` is 30 seconds of real Binance BTCUSDT level-2 data (301 depth diffs, 879 trades and 10
REST snapshots), small enough to commit (0.5 MB). It lets CI and any reader run the real-data validation without
downloading anything:

```bash
build/release/exsim_mdreplay --in data/sample_btcusdt_30s.exmd
```

The longer sessions behind the market-making study (30 min of BTCUSDT, 10+ min of ETHUSDT) are tens of MB and are not
committed. Re-capture your own with the scripts below (Binance's public market-data endpoints, no API key). The results in
`results/` were produced from those captures, and the grid is deterministic given a capture.

## Capturing and converting

```bash
pip install websockets
python scripts/capture_binance.py --symbol BTCUSDT --minutes 30 --out data/btcusdt.jsonl   # JSONL, one record per line
python scripts/mdconv.py data/btcusdt.jsonl data/btcusdt.exmd                              # exact integer ticks / lots
```

## The `.exmd` format

All decimal scaling happens once, in `mdconv.py`, with exact decimal arithmetic (it refuses values that are not
exactly representable). Prices are integer ticks (1e-2 for BTCUSDT) and quantities integer lots (1e-5), so nothing
downstream touches floating point.

```
header  "EXMD" | u32 version=1 | u32 price_decimals | u32 qty_decimals
record  u32 payload_len | u8 kind | u8 pad[3] | u64 rx_ns | body        (little endian)
  kind 1 diff      u64 first_id, u64 last_id, u32 n_bids, u32 n_asks, then {i64 px, u64 qty} x (n_bids + n_asks)   qty 0 = delete
  kind 2 trade     i64 px, u64 qty, u8 buyer_is_maker
  kind 3 snapshot  u64 lastUpdateId, u32 n_bids, u32 n_asks, then {i64 px, u64 qty} x (n_bids + n_asks)
```

`rx_ns` is the **local receive time**, and it is the only clock used for everything. Mixing the exchange's trade
timestamps with local book-update timestamps skews every fill-relative measurement (the two clocks differ by seconds),
so the capture never uses exchange timestamps.

Diffs and trades arrive on the same websocket connection, so `rx_ns` orders them correctly relative to each other.
Snapshots are fetched over REST every 2 s **after** the websocket is open (Binance's documented procedure), and their
`lastUpdateId` lines up exactly with a diff boundary in every case observed, which is what makes exact validation possible.

# Coinbase order-by-order (level 3) data

The engine-versus-exchange validation ([docs/VALIDATION.md](../docs/VALIDATION.md)), the queue-position study and the
signal study use Coinbase Exchange's BTC-USD "full" channel: every order the exchange received, and every rest, match,
modify and cancel, each with its order id. [Tardis.dev](https://tardis.dev) archives it and serves the first day of every
month without an API key. A day is 1.5 to 4.5 GB compressed, so none of it is committed; CI fetches the first 5
minutes of a day and validates those.

```bash
pip install orjson                                   # optional: faster JSON parsing in the converter
python scripts/fetch_coinbase_l3.py --date 2026-09-01 --hours 24 --out data/l3/2026-09-01   # resumable, hourly .gz
python scripts/l3conv.py data/l3/2026-09-01 data/l3/2026-09-01.exl3                        # ~20 minutes per day
build/release/exsim_l3replay   --in data/l3/2026-09-01.exl3                                 # engine vs Coinbase
build/release/exsim_queuestudy --in data/l3/2026-09-01.exl3 --out-prefix results/queue/2026-09-01
build/release/exsim_features   --in data/l3/2026-09-01.exl3 --out-prefix results/signals/2026-09-01
```

A capture must start at 00:00 UTC: that is where Tardis records a full book snapshot (`full_snapshot`), the only way to
seed the book. It records another whenever its capture connection reconnects, which is how the tools recover from the
occasional gap in the archived sequence numbers.

## The `.exl3` format

`l3conv.py` scales prices to integer ticks (1e-2 USD) and sizes to integer satoshis (1e-8 BTC) with exact string
arithmetic, maps each order's UUID to a sequential 64-bit id, and writes every message as one record, in file order.

```
header  "EXL3" | u32 version=1 | u32 price_decimals | u32 qty_decimals
record  72 bytes: u64 seq | u64 exchange_time_ns | u64 id | u64 id2 | i64 px | i64 px2 | u64 q | u64 q2 |
                  u8 kind | u8 side | u8 order_type | u8 flags | u32 pad                        (little endian)
  kind   1 snapshot order   2 snapshot end   3 received   4 open   5 done   6 match   7 change
```

UUIDs are forgotten only a million messages after an order finishes: the snapshot is fetched while the stream runs,
so an order's `done` can appear in the file before a snapshot that still lists it (see DESIGN.md section 8).
