# Does the engine match like a real exchange?

A matching engine's unit tests check it against the author's reading of the rules. This document checks it against an
exchange: twelve full days of Coinbase Exchange's BTC-USD order-by-order feed (791 million messages), replayed through the
engine and compared, trade by trade, with what Coinbase actually did.

**Result: 284,539,832 of 284,539,832 arrivals and modifies, and 7,503,266 of 7,503,266 trades, reproduced exactly.**
Every divergence met on the way there turned out to be a rule of the feed or a bug in the replay (both listed below),
not a matching error in the engine. The engine itself changed twice for this work: quantities became 64-bit (real sizes
in satoshis overflow 32 bits) and self-trade prevention gained decrement-and-cancel, selectable per order.

## Method

Coinbase's "full" channel publishes every order event with its order id: `received` (an order arrived), `open` (it rests),
`match` (a trade, naming taker and maker), `change` (a modify), `done` (filled or cancelled). Tardis.dev archives it, free
for the first day of each month. `tools/exsim_l3replay.cpp` runs two books side by side:

- the **truth book** (`l3.hpp`) applies Coinbase's messages literally. It has no matching logic: an order joins the back of
  its level when Coinbase says `open`, shrinks when a `match` names it, leaves on `done`;
- the **engine** receives only what a matching engine would: new orders, cancels and modifies. It does its own matching.

Every arrival and every price or size modify is an **episode**. The engine's trades (which makers, at which prices, in which
order, for how much) and its resting remainder are compared with Coinbase's `match` and `open` messages, exactly. Separately,
every 500,000 messages the entire engine book is compared with the truth book, order by order in queue order. A divergence
is repaired from the truth book at the affected price levels and counted, so one unexplained event cannot cascade into the
rest of the day.

**Inputs the feed does not carry are inferred, and counted.** Time in force: an order that never rested was IOC or FOK.
Post-only: an order that never rested and never traded. The size of a funds-denominated market order: what it filled.
Accounts, for self-trade prevention: Coinbase never names accounts, but prevention leaves traces inside an arrival's
processing block (Coinbase processes an arrival atomically):

| Evidence in the block | Coinbase mode | What the engine is given |
|---|---|---|
| another order is cancelled | cancel oldest (`co`) | the two orders share an owner; the arrival cancels resting |
| another order is cancelled, then the arrival is cancelled with size left, having traded nothing since | cancel both (`cb`) | shared owner; cancel incoming (the resting order's own `done` removes it) |
| an STP `change` decrements an order | decrement and cancel (`dc`) | shared owner; decrement-and-cancel |
| the arrival traded, then was cancelled while liquidity at its limit remained | cancel newest (`cn`) | shared owner with the next order in queue priority; cancel incoming |

The last inference needs the book at that moment, so the first pass over the file keeps a truth book too.

## Results

Twelve days, the first of each month from October 2025 to September 2026, one binary, no per-day tuning
(`results/l3/`, produced by `scripts/summarize_l3.py`):

| day | messages | feed gaps (recovered) | episodes | reproduced exactly | diverged | Coinbase trades | predicted exactly | full-book checks (failed) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 2025-10-01 | 56,049,823 | 1 (1) | 20,929,642 | 20,929,642 | 0 | 486,910 | 486,910 | 112 (0) |
| 2025-11-01 | 39,992,754 | 0 | 14,433,050 | 14,433,050 | 0 | 233,833 | 233,833 | 79 (0) |
| 2025-12-01 | 101,277,262 | 10 (10) | 37,195,766 | 37,195,766 | 0 | 786,118 | 786,118 | 202 (0) |
| 2026-01-01 | 21,014,690 | 0 | 7,715,830 | 7,715,830 | 0 | 320,115 | 320,115 | 42 (0) |
| 2026-02-01 | 81,192,370 | 2 (2) | 30,184,925 | 30,184,925 | 0 | 744,183 | 744,183 | 162 (0) |
| 2026-03-01 | 81,200,901 | 0 | 29,135,676 | 29,135,676 | 0 | 621,229 | 621,229 | 162 (0) |
| 2026-04-01 | 79,238,887 | 2 (2) | 28,159,155 | 28,159,155 | 0 | 738,670 | 738,670 | 158 (0) |
| 2026-05-01 | 53,590,545 | 1 (1) | 19,048,354 | 19,048,354 | 0 | 846,726 | 846,726 | 107 (0) |
| 2026-06-01 | 83,913,766 | 11 (11) | 29,652,716 | 29,652,716 | 0 | 776,578 | 776,578 | 167 (0) |
| 2026-07-01 | 84,759,322 | 3 (3) | 30,073,362 | 30,073,362 | 0 | 1,003,646 | 1,003,646 | 169 (0) |
| 2026-08-01 | 42,758,642 | 0 | 14,651,090 | 14,651,090 | 0 | 352,014 | 352,014 | 85 (0) |
| 2026-09-01 | 66,038,550 | 10 (10) | 23,360,266 | 23,360,266 | 0 | 593,244 | 593,244 | 131 (0) |
| **all** | **791,027,512** | **40 (40)** | **284,539,832** | **284,539,832** | **0** | **7,503,266** | **7,503,266** | **1,576 (0)** |

Every one of 284,539,832 arrivals and modifies produced exactly the trades Coinbase printed (same makers, prices,
sizes, order) and left exactly the resting order Coinbase reported: 243,223,942 resting remainders were checked
against Coinbase's `open` messages, and none differed. All 7,503,266 trades were predicted. The engine book equalled the
truth book, order for order, at every one of 1,576 whole-book comparisons. The truth book itself never contradicted
the feed (0 inconsistencies), and no match ever named an unknown taker.

What the feed does not state was inferred and counted, over the twelve days: 3,962,790 arrivals treated as IOC/FOK (never
rested), 1,853,119 as post-only (never rested, never traded), 89,243 funds-denominated market orders sized by what they
filled, and 64,639 account links from self-trade prevention, giving 40,100 arrivals a prevention mode (including 10,919
cancel-both and 66 cancel-newest cases) and explaining 5,583 prevention decrements.

## What is not scored, and why

Getting to zero divergences did not involve excluding anything that could diverge. These are the only places where the
replay does not compare:

- **40 gaps in the archive** (a few hundred messages each, Coinbase or Tardis losing part of the stream). The episode
  open at each gap (40 in total) is not scored, and the replay waits for Tardis's next snapshot. The 120,089 messages
  between each gap and its snapshot are represented by the snapshot rather than replayed.
- **1,403 orders resting beyond the engine's $0 to $167,772 band** (bids at a fraction of the price and asks at many times
  it). The engine cannot hold them. They never traded, which is why no divergence came from them; if one had, it would
  have been counted as one.
- **41 orders the exchange reported "filled" whose published matches do not account for their size** (0.96 BTC in total
  over the twelve days, 0 to 13 orders a day). The raw JSON confirms each: for example a buy of 0.25861818 BTC is matched
  for 0.15846986, and ten milliseconds later Coinbase reports it filled with remaining size 0, with no match and no
  sequence gap in between. Where the rest went is not visible in the public feed. The replay treats the exchange's
  statement as final and removes the remainder from the engine too.

## Rules of the feed, learned from divergences

Each of these was a wrong prediction first:

| Divergence | What Coinbase actually does | How the replay handles it |
|---|---|---|
| Orders missing just after the snapshot | The snapshot is fetched while the stream runs, so messages newer than it appear before it in the file | Buffer pre-snapshot messages; replay those with a later sequence number |
| 716 truth-book inconsistencies on modifies | A crossing modify reports its matches before the `change`, whose size is the post-trade remainder; a modify that fills completely sends no `change` | Hold the matches until the `change`, or the `done` |
| Ghost orders that never cancel | (my bug) the converter forgot an order's UUID on `done`, but a `done` can precede a snapshot that still lists the order | Forget UUIDs only a million messages later |
| A day that ended after 5 hours | The archive has gaps (107 to 486 messages); Tardis records a new snapshot when it reconnects | Stop scoring at a gap, resynchronize from the next snapshot |
| Arrivals cancelling another order without a trade | Self-trade prevention, in all four of Coinbase's modes | Infer accounts and modes (above); decrement-and-cancel added to the engine |
| An arrival that filled at one level and stopped with the next level still crossing | Cancel newest: its next maker was its own order | The first pass keeps a truth book to see the liquidity that was left |
| A market sell of 1,600 that cancelled an own order and then itself, trading nothing | Cancel both | Detected when nothing traded after the first own-order cancel (cancel oldest keeps matching) |
| Funds-denominated market buys trading 198 lots short | Decrement-and-cancel reduces a funds order's *funds*; the filled size already reflects it | The engine decrements size, so its input adds back the size of the own orders cancelled |
| Matches of a resting order counted as its arrival's trades | (my bug) an order rested, then was modified across the spread before anything else arrived; its matches were attributed to the arrival | An arrival's episode ends when the order rests |
| A resting buy at $78,357.62 trading as a taker at $78,364.24 | A fill-modify to $783,560 (a fat finger), beyond any sensible band | Clamp to the band edge when it fills and never rests, exactly as for arrivals |
| Sell orders at $0.01, modifies to a tenth of the price | Real orders far from the market | A 2^24-tick band ($0 to $167,772); orders beyond it that never rest are clamped, others counted |
| A sell of 0.03 BTC matched for 0.02882596, then reported "filled" | The exchange's own messages do not add up (no gap: the size vanishes) | The exchange's word is final: the engine drops the remainder too; each case is counted |

## Can the check fail?

Yes, and CI shows it on every push. It fetches the first five minutes of a day, requires 100% agreement, then drops every
1,000th cancel before it reaches the engine: the ghost orders this leaves must produce divergences and failed whole-book
comparisons, and they do (8 divergences, 14 missed trades and 9 of 10 failed comparisons on the CI sample).

## What this does and does not establish

It establishes that the engine's price-time priority, partial fills, IOC and market behaviour, modify semantics (including
the priority rules and crossing modifies) and self-trade prevention agree with a production exchange across 284,539,832
episodes on twelve days spread over a year. It does not establish behaviour Coinbase does not exercise on BTC-USD (FOK is rare, there are no
auctions), and the inferred inputs, while counted, are inferences: an episode reproduced exactly with an inferred time in force
shows the inference is consistent with Coinbase, not that it is the order's true instruction.

## Reproducing

```bash
for d in 2025-10-01 2025-11-01 2025-12-01 2026-01-01 2026-02-01 2026-03-01 2026-04-01 2026-05-01 2026-06-01 2026-07-01 2026-08-01 2026-09-01; do
  python scripts/fetch_coinbase_l3.py --date $d --hours 24 --out data/l3/$d
  python scripts/l3conv.py data/l3/$d data/l3/$d.exl3
  build/release/exsim_l3replay --in data/l3/$d.exl3 --check-every 500000 > results/l3/$d.txt
done
python scripts/summarize_l3.py results/l3/*.txt --out results/l3/summary.json
```

`--trace` prints, for each divergence, the lifecycle facts of the taker and makers and the messages around it.
The per-day reports are committed in [results/l3/](../results/l3/).
