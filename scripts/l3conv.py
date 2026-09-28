#!/usr/bin/env python3
"""Convert a Tardis Coinbase full-channel capture (fetch_coinbase_l3.py output) to the binary .exl3 format.

All decimal scaling happens here, exactly (string arithmetic, no floats): prices become integer ticks
(1e-2 USD) and sizes integer satoshis (1e-8 BTC). Order UUIDs become sequential 64-bit ids. Nothing is
filtered or reordered: every message becomes one record, in file order.

    python l3conv.py data/l3/2026-09-01 data/l3/2026-09-01.exl3

.exl3 layout (little endian):
  header  "EXL3" | u32 version=1 | u32 price_decimals | u32 qty_decimals
  record  72 bytes: u64 seq | u64 exchange_time_ns | u64 id | u64 id2 | i64 px | i64 px2 | u64 q | u64 q2 |
                    u8 kind | u8 side | u8 order_type | u8 flags | u32 pad
  kind   1 snapshot order (id, side, px, q) in the snapshot's queue order   2 snapshot end (seq = snapshot sequence)
         3 received (id, side, order_type, px limit price or 0, q size or 0)  4 open (id, side, px, q remaining)
         5 done (id, side, px or 0, q remaining)   6 match (id taker, id2 maker, side = maker side, px, q)
         7 change (id, side, px new price, px2 old price, q new size, q2 old size)
  flags  1 funds-only market order (no size)   2 done: canceled (else filled)   4 change: self-trade prevention
         8 change: funds change (no sizes)     16 done: had a price
"""
import collections, glob, gzip, os, struct, sys, datetime as dt

try:
    import orjson
    loads = orjson.loads
except ImportError:  # pragma: no cover
    import json
    loads = json.loads

REC = struct.Struct("<QQQQqqQQBBBB4x")
assert REC.size == 72
SNAP_ORDER, SNAP_END, RECEIVED, OPEN, DONE, MATCH, CHANGE = range(1, 8)
F_FUNDS, F_CANCELED, F_STP, F_FUNDS_CHANGE, F_HAS_PRICE = 1, 2, 4, 8, 16
PRICE_DEC, QTY_DEC = 2, 8


def scaled(s, dec):
    """Exact decimal string -> integer at `dec` decimals; refuses anything not exactly representable."""
    neg = s.startswith("-")
    if neg:
        s = s[1:]
    ip, _, fp = s.partition(".")
    fp = fp.rstrip("0")
    if len(fp) > dec:
        raise ValueError(f"{s} has more than {dec} decimals")
    v = int(ip or "0") * 10 ** dec + int((fp or "0").ljust(dec, "0"))
    return -v if neg else v


_day_cache = {}


def time_ns(s):
    # "2026-09-01T13:00:00.201235Z" or with 9 fractional digits
    d, t = s[:10], s[11:].rstrip("Z")
    base = _day_cache.get(d)
    if base is None:
        base = _day_cache[d] = int(dt.datetime.fromisoformat(d).replace(tzinfo=dt.timezone.utc).timestamp()) * 10 ** 9
    hms, _, frac = t.partition(".")
    h, m, sec = hms.split(":")
    return base + (int(h) * 3600 + int(m) * 60 + int(sec)) * 10 ** 9 + int((frac or "0").ljust(9, "0")[:9])


def main():
    src, dst = sys.argv[1], sys.argv[2]
    files = sorted(glob.glob(os.path.join(src, "*.txt.gz")))
    if not files:
        sys.exit(f"no .txt.gz files in {src}")
    ids = {}
    next_id = [1]
    # UUIDs of finished orders are forgotten only after a long delay. The snapshot is fetched while the stream
    # runs, so an order's `done` can appear in the file BEFORE a snapshot that still (correctly) lists it:
    # forgetting the UUID immediately would give that snapshot entry a fresh id that no later message
    # refers to, leaving a ghost order in the book. (That bug was caught by exsim_l3replay: the ghost made a
    # real resting order look like it had rested across the spread.)
    retire = collections.deque()
    RETIRE_AFTER = 1_000_000
    n_msgs = 0

    def oid(u):
        v = ids.get(u)
        if v is None:
            v = ids[u] = next_id[0]
            next_id[0] += 1
        return v

    counts = [0] * 8
    with open(dst, "wb") as out:
        out.write(b"EXL3" + struct.pack("<III", 1, PRICE_DEC, QTY_DEC))
        buf = bytearray()
        for path in files:
            with gzip.open(path, "rb") as f:
                for line in f:
                    if not line.strip():  # an empty minute can leave a blank line between minutes
                        continue
                    sp = line.find(b" ")
                    m = loads(line[sp + 1:])
                    t = m.get("type")
                    side = 1 if m.get("side") == "sell" else 0
                    if t == "received":
                        flags = 0
                        q = scaled(m["size"], QTY_DEC) if "size" in m else 0
                        if "size" not in m:
                            flags |= F_FUNDS
                        px = scaled(m["price"], PRICE_DEC) if "price" in m else 0
                        rec = (m["sequence"], time_ns(m["time"]), oid(m["order_id"]), 0, px, 0, q, 0,
                               RECEIVED, side, 0 if m["order_type"] == "limit" else 1, flags)
                    elif t == "open":
                        rec = (m["sequence"], time_ns(m["time"]), oid(m["order_id"]), 0, scaled(m["price"], PRICE_DEC), 0,
                               scaled(m["remaining_size"], QTY_DEC), 0, OPEN, side, 0, 0)
                    elif t == "done":
                        flags = (F_CANCELED if m.get("reason") == "canceled" else 0) | (F_HAS_PRICE if "price" in m else 0)
                        u = m["order_id"]
                        rs = m.get("remaining_size")
                        rec = (m["sequence"], time_ns(m["time"]), oid(u), 0,
                               scaled(m["price"], PRICE_DEC) if "price" in m else 0, 0,
                               scaled(rs, QTY_DEC) if rs is not None else 0, 0, DONE, side, 0, flags)
                        retire.append((n_msgs, u))  # no more messages for this order (see RETIRE_AFTER)
                    elif t == "match":
                        rec = (m["sequence"], time_ns(m["time"]), oid(m["taker_order_id"]), oid(m["maker_order_id"]),
                               scaled(m["price"], PRICE_DEC), 0, scaled(m["size"], QTY_DEC), 0, MATCH, side, 0, 0)
                    elif t == "change":
                        flags = F_STP if m.get("reason") == "STP" else 0
                        if "new_size" in m:
                            q, q2 = scaled(m["new_size"], QTY_DEC), scaled(m["old_size"], QTY_DEC)
                        else:
                            q = q2 = 0
                            flags |= F_FUNDS_CHANGE
                        px = m.get("new_price", m.get("price"))
                        px2 = m.get("old_price", m.get("price"))
                        rec = (m["sequence"], time_ns(m["time"]), oid(m["order_id"]), 0,
                               scaled(px, PRICE_DEC) if px else 0, scaled(px2, PRICE_DEC) if px2 else 0, q, q2,
                               CHANGE, side, 0, flags)
                    elif t == "full_snapshot":
                        seq, ts = m["sequence"], time_ns(m["time"])
                        for sname, sd in (("bids", 0), ("asks", 1)):
                            for p, s, u in m[sname]:
                                buf += REC.pack(seq, ts, oid(u), 0, scaled(p, PRICE_DEC), 0, scaled(s, QTY_DEC), 0,
                                                SNAP_ORDER, sd, 0, 0)
                                counts[SNAP_ORDER] += 1
                        rec = (seq, ts, 0, 0, 0, 0, 0, 0, SNAP_END, 0, 0, 0)
                    else:
                        continue
                    buf += REC.pack(*rec)
                    counts[rec[8]] += 1
                    n_msgs += 1
                    while retire and retire[0][0] < n_msgs - RETIRE_AFTER:
                        ids.pop(retire.popleft()[1], None)
                    if len(buf) > (8 << 20):
                        out.write(buf)
                        buf.clear()
            print(f"{os.path.basename(path)} done, {sum(counts):,} records so far", flush=True)
        out.write(buf)
    names = ["", "snapshot orders", "snapshots", "received", "open", "done", "match", "change"]
    print(", ".join(f"{names[i]} {counts[i]:,}" for i in range(1, 8)))


if __name__ == "__main__":
    main()
