# Research on real order-book data

Three studies, each built on the engine and on data validated against the exchange that produced it.

1. [Passive market making](#part-1-passive-market-making-binance-l2) on an hour of Binance BTC and ETH level-2 data.
2. [Queue position, measured](#part-2-queue-position-measured-coinbase-l3): where cancellations really come from, and which
   level-2 queue model predicts fills best, scored against twelve days of Coinbase order-by-order ground truth.
3. [Order-book signals](#part-3-order-book-signals-coinbase-l3): queue imbalance and order-flow imbalance, tested out of
   sample across days with a correction for multiple testing.

# Part 1: passive market making (Binance L2)

**Question.** Can a passive quoting strategy make money on liquid crypto spot books once fills account for queue
position and latency and fees are included, and does the Avellaneda-Stoikov (A-S) model beat simpler quoting?

**Short answer.** No, not on these data. Every strategy loses about 1.3 to 2 basis points of the notional it trades, because
the fills it gets are disproportionately the ones where the price is about to move against it. A-S is an inventory-control
tool: it loses less by trading less, and its edge over an equal-width fixed spread is statistically indistinguishable from
zero in all but one cell. A naive "touch means filled" backtest overstates fills by up to 7x. Fees at retail levels swamp
everything.

That is a result about ~1 hour of data on two assets, not a law. The [limits](#what-this-does-not-show) section is part of
the result.

## Data

Live Binance spot data over public endpoints, recorded by `scripts/capture_binance.py` against **one local clock**: 100 ms
depth diffs, every trade, and a REST snapshot every 2 s (websocket opened first, per Binance's book-management procedure).

| Session | Asset | Length | Diffs | Trades | Snapshots checked | Book validation |
|---|---|---:|---:|---:|---:|---|
| `btcusdt_a` | BTCUSDT | 30 min | 17,997 | 57,885 | 573 | 573/573 exact, 458,400 levels |
| `ethusdt_b` | ETHUSDT | 10 min | 6,028 | 12,655 | 168 | 168/168 exact, 134,400 levels |
| `ethusdt_c` | ETHUSDT | 25 min | 14,951 | 55,055 | 418 | 418/418 exact, 334,400 levels |

The engine's reconstructed book equals the exchange's own snapshot, level for level (price and quantity), at the exact
sequence point (`lastUpdateId`), with 0 phantom trades and 0 sequence gaps, in every session. The check is shown to be able
to fail: dropping 1 in 100,000 level updates (6 of 648,000) fails 8 BTC snapshots; 1 in 2,000 fails 441 of 573. Details and
the two ways it initially failed are in [DESIGN.md section 8](DESIGN.md#8-does-the-engine-match-like-a-real-exchange).

## Method

**Book.** The exchange book is replayed through the matching engine (one synthetic order per price level). Quotes are
*virtual*: they are never inserted, because level quantities are absolute totals and inserting our size would double-count it.

**Fill model** (`include/exsim/mm.hpp`, unit-tested on hand-computed cases):
- On going live a quote joins the **back** of the displayed queue: `ahead = level quantity`.
- A trade at our price consumes `ahead` first; only the excess fills us. A trade *through* our price fills us in full.
- Level shrinkage that trades do not explain is cancellation. A canceller's queue position is unobservable in L2 data, so it
  is prorated by queue share: `ahead -= cancelled * ahead / (level - traded)`. Level growth joins behind us.
- If the market moves to or through our price we are filled in full.
- `--fill optimistic` ignores the queue (any trade at or through the price fills us), to measure how much that overstates.

**Latency** is decision-to-market, both for placement and for cancels, with amend-in-flight (a re-quote before the previous
one is live keeps the original effective time). **Fees** are basis points of notional per fill; negative is a rebate.
**Re-quote hysteresis** (2 ticks) stops strategies from forfeiting queue position on 1-tick wobbles.

**Strategies.**
- *Avellaneda-Stoikov*: reservation `r = mid - q*gamma*sigma^2*tau`, half spread `0.5*gamma*sigma^2*tau + ln(1 + gamma/k)/gamma`,
  `sigma^2` an EWMA variance rate (10 s half-life), `tau` a fixed 30 s horizon (a constant, the standard adaptation for a
  continuously running desk), inventory in quote-size units, capped at 10 units.
- *Fixed spread* at `h` ticks each side, same size and cap. One is set to `1/k` ticks, **as wide as A-S's own quotes**: the
  fair control, since otherwise A-S's width, not its inventory skew, would drive any difference.
- *Join touch*: quote the best bid and ask.

**Calibrating k.** A-S assumes the rate a quote at distance `d` from the mid is hit is `A*exp(-k*d)`. A quote at distance `d`
is reached by every trade that hits a resting order at depth >= `d`, so `lambda(d)` is the empirical survival rate of trade
depth. `scripts/calibrate_k.py` fits it from the same capture:

| Session | k (per tick) | 1/k | exponential R^2 | median / p90 trade depth (ticks) |
|---|---:|---:|---:|---|
| BTCUSDT | 0.00243 | 411 | 0.98 | 0.5 / 526 |
| ETHUSDT (10 min) | 0.0584 | 17 | 0.996 | 1.5 / 25.5 |
| ETHUSDT (25 min) | 0.0289 | 35 | 0.95 | 4.5 / 39.5 |

k differs 12- to 24-fold between BTC and ETH (ETH's fixed $0.01 tick is ~30x coarser relative to price), and **by 2x between two
sessions of the same asset**, so a calibrated k is a snapshot of a regime, not a constant. The BTC fit is good in the tail but
the measured intensity is flat out to ~50 ticks (`docs/img/k_calibration_btcusdt_a.png`): A-S's exponential form is a local
approximation, and trade depth is clustered (one sweep produces many fills), which a Poisson model ignores.

**Grid, fixed before looking at results.** Risk aversion is swept in the dimensionless ratio `gamma/k` in
{4e-4, 4e-3, 4e-2, 4e-1} so different tick sizes get the same principled grid; latency in {0, 10, 50, 200} ms; fees in
{+10, +1, 0, -0.5} bps; fill model in {queue, optimistic}. All 72 runs per session are in `results/*/grid.csv`.

**Statistics.** PnL increments are strongly autocorrelated, so a naive standard error would be far too small. Intervals are
**moving-block bootstrap** on 30-second increments (4,000 resamples); for a strategy difference the blocks are **paired**
(same time windows for both). The accounting is audited by `scripts/verify_accounting.py`, which recomputes every run's PnL,
inventory and spread capture from the raw fill logs with no shared code; all runs reconcile to within 0.02 USDT (0.0001 on BTC).

## Results

Main comparison: 10 ms latency, no fees, queue-aware fills, 95% block-bootstrap interval on total PnL (USDT).

### BTCUSDT, 30 minutes (quote size 0.01 BTC)

| Strategy | Fills | Volume (BTC) | Spread capture | Inventory PnL | **Total** | 95% CI | bps of notional | Mean \|inv\| (BTC) |
|---|---:|---:|---:|---:|---:|---|---:|---:|
| touch | 1,246 | 4.39 | -0.29 | -51.25 | **-51.54** | [-76.3, -27.7] | -1.40 | 0.074 |
| fixed h=2 | 512 | 2.97 | -0.32 | -44.19 | **-44.51** | [-71.8, -18.8] | -1.78 | 0.067 |
| fixed h=10 | 297 | 2.91 | -0.09 | -45.00 | **-45.08** | [-72.0, -19.2] | -1.84 | 0.067 |
| fixed h=100 | 297 | 2.76 | 2.37 | -39.46 | **-37.09** | [-62.2, -14.2] | -1.60 | 0.065 |
| fixed h=412 (= A-S width) | 201 | 1.99 | 7.45 | -34.17 | **-26.72** | [-46.1, -9.1] | -1.60 | 0.055 |
| A-S gamma/k=4e-4 | 217 | 2.01 | 7.46 | -32.09 | **-24.63** | [-44.4, -6.5] | -1.46 | 0.050 |
| A-S gamma/k=4e-3 | 231 | 2.02 | 7.25 | -32.47 | **-25.22** | [-44.4, -10.2] | -1.48 | 0.033 |
| A-S gamma/k=4e-2 | 164 | 1.31 | 4.26 | -22.45 | **-18.19** | [-27.1, -11.2] | -1.65 | 0.012 |
| A-S gamma/k=4e-1 | 66 | 0.58 | 1.96 | -8.92 | **-6.96** | [-9.9, -4.4] | -1.44 | 0.005 |

Paired difference, A-S minus the equal-width fixed spread (fixed h=412), total PnL over the session:

| A-S gamma/k | difference (USDT) | 95% paired CI |
|---:|---:|---|
| 4e-4 | +2.1 | [-4.5, +7.8] |
| 4e-3 | +1.5 | [-12.0, +13.6] |
| 4e-2 | +8.5 | [-6.4, +23.5] |
| 4e-1 | **+19.8** | **[+2.5, +38.2]** |

### ETHUSDT, 25 minutes (quote size 0.01 ETH, about $27)

| Strategy | Fills | Spread capture | Inventory PnL | **Total** | 95% CI | bps of notional |
|---|---:|---:|---:|---:|---|---:|
| touch | 1,438 | 0.03 | -3.04 | **-3.01** | [-5.0, -1.4] | -1.31 |
| fixed h=2 | 1,439 | 0.10 | -3.64 | **-3.53** | [-5.8, -1.7] | -2.09 |
| fixed h=10 | 914 | 0.41 | -2.89 | **-2.48** | [-4.6, -0.7] | -1.97 |
| fixed h=35 (= A-S width) | 192 | 0.38 | -2.14 | **-1.75** | [-3.9, +0.1] | -5.19 |
| fixed h=100 | 11 | 0.10 | -0.33 | **-0.22** | [-1.0, +0.5] | -7.49 |
| A-S gamma/k=4e-4 | 227 | 0.40 | -2.10 | **-1.70** | [-3.5, -0.2] | -4.60 |
| A-S gamma/k=4e-3 | 221 | 0.41 | -1.33 | **-0.92** | [-1.9, -0.2] | -2.42 |
| A-S gamma/k=4e-2 | 95 | 0.21 | -0.48 | **-0.27** | [-0.5, -0.1] | -1.34 |
| A-S gamma/k=4e-1 | 27 | 0.06 | -0.14 | **-0.09** | [-0.16, -0.03] | -1.49 |

Every paired A-S-vs-fixed(35) interval includes zero (best case +1.7, CI [-0.2, +3.8]). The 10-minute ETH session shows the same
pattern (`results/ethusdt_b/`).

## Findings

1. **Passive quoting loses, and the loss is adverse selection, not bad execution.** Spread capture is positive but tiny
   (cents to a few dollars); inventory PnL is negative in every run. One second after a BTC fill the mid has moved against the
   position by $4.7 to $10. Unconditionally, BTC's 1-second mid change in this session has a standard deviation of $3.49 and a
   mean absolute value of only $1.12, so a fresh fill is followed by a move 4 to 9 times the typical one, in the wrong
   direction. Thirty seconds after a fill the mid is $9 to $14 against the position (unconditional 30 s standard deviation:
   $25). Fills are not a random sample of time: they are the moments someone aggressive chose to trade.

2. **The loss per dollar traded is nearly design-independent.** On BTC every strategy loses 1.4 to 1.8 bps of notional; on
   ETH the near-touch strategies lose 1.3 to 2.1 bps. Deep quotes on ETH show larger bps (4 to 7) but on 11 to 200 fills, so they
   are noisy. It follows that only a **maker rebate above roughly 1.5 bps** or a genuine information edge could make this
   profitable; the tested -0.5 bps rebate still loses on every strategy and asset.

3. **Fees dominate.** At 10 bps of notional (Binance's default retail maker rate) BTC losses become $55 (A-S at high risk
   aversion) to $421 (touch quoting) in 30 minutes. At 1 bps they raise the zero-fee loss by roughly 60 to 70%.

4. **A-S wins by trading less, not by earning more.** Its inventory is far smaller (mean 0.005 to 0.05 BTC vs 0.055 to 0.074)
   and its total loss is smaller at high risk aversion, but at gamma/k = 4e-1 it also trades 0.58 BTC against 1.99. On BTC that
   cell is the only significant improvement over an equal-width fixed spread (+19.8, CI [+2.5, +38.2]). With 4 gamma values
   and 2 assets, some cell being nominally significant is not surprising, and it is not replicated on ETH (CI includes 0).
   Read it as risk reduction.

5. **A naive fill model overstates activity, and sometimes results.** For touch quoting, "any trade at the price fills me"
   yields 8,693 BTC fills against the queue-aware 1,246 (7.0x) and a PnL of -41.4 against -51.5. On ETH: 5,281 vs 1,438 fills
   (3.7x). For fixed quotes 10 or more ticks back the two models agree (the queue ahead is short or empty). For actively
   re-quoting A-S at high risk aversion the optimistic model still gives 53% more fills (251 vs 164 on BTC), because the
   queue-aware model charges for the queue position lost at every re-quote. The model matters most where retail backtests
   tend to quote: at the touch.

6. **Latency barely matters here, which is itself informative.** Total PnL changes by a few dollars from 0 to 200 ms decision
   latency (touch: -51.3 at 0 ms, -51.5 at 10 ms, -51.7 at 50 ms, -44.3 at 200 ms; the 200 ms improvement is fewer fills, not
   better ones). The strategies react to 100 ms book updates, so extra delay mostly stales quotes that were adversely selected
   anyway. A strategy that predicted short-horizon flow would be the case where latency matters, and this study has none.

7. **A calibrated parameter is a regime, not a constant.** k moved 25x between assets and 2x between two ETH sessions.

## What this does not show

- **One hour of data, two assets.** The BTC session trended up. A different regime (range-bound, or a volatility spike) could
  change the sign of inventory PnL. Intervals are wide; three of nine BTC strategies have a CI that comes near zero.
- **Model risk in the fill model.** The cancellation proration is a neutral assumption; part 2 measures it against
  order-by-order ground truth (on a different venue) and reruns the grid with the rule that fits best. No hidden or iceberg orders, no own
  market impact (our fills would have moved the book that produced them), book data is 100 ms granular (so "mid at fill" can be
  stale by up to 100 ms), and queue position is estimated, not observed. The optimistic-vs-queue gap brackets the effect
  but does not bound it.
- **Grid, not optimization.** gamma/k was swept and every cell is reported; nothing was tuned on the data. But the strategy
  set is small and there is no signal. This is a study of *quoting policy*, not of alpha.
- **Fees are a constant per fill.** Real tiers depend on volume.
- **Single venue, no hedging.** Inventory is marked to mid and never flattened.

## Reproducing it

```bash
pip install websockets numpy pandas matplotlib
python scripts/capture_binance.py --symbol BTCUSDT --minutes 30 --out data/btcusdt_a.jsonl
python scripts/mdconv.py data/btcusdt_a.jsonl data/btcusdt_a.exmd
build/release/exsim_mdreplay --in data/btcusdt_a.exmd                     # exact validation
build/release/exsim_mmsim --in data/btcusdt_a.exmd --calibrate depth.csv
python scripts/calibrate_k.py depth.csv --plot docs/img/k.png             # -> k
python scripts/run_experiments.py --bin build/release/exsim_mmsim --data data/btcusdt_a.exmd:0.00243 --out results/btcusdt_a
python scripts/verify_accounting.py --results results/btcusdt_a --dataset btcusdt_a
python scripts/analyze_results.py --results results/btcusdt_a --img docs/img --dataset btcusdt_a
```

A fresh capture will not reproduce these numbers exactly (the market is different); the committed `results/` and the
committed 30-second sample let every stage be re-run without network access. Rerunning the whole grid after later engine
changes (64-bit quantities, the Robin Hood index) reproduced all 216 committed runs exactly: every number in every row
is identical.

# Part 2: queue position, measured (Coinbase L3)

**Question.** Every level-2 backtest, including part 1's, must guess where a cancellation came from: when a price level
shrinks without a trade, was the cancelled quantity ahead of your order or behind it? The answer decides when your order
fills. Order-by-order data makes the queue observable, so the guess can be scored.

**Data.** Twelve full days of Coinbase BTC-USD (`exsim_queuestudy`), the same days as [VALIDATION.md](VALIDATION.md),
where the engine reproduces the exchange exactly. The truth book is Coinbase's own queue.

**Where cancellations come from.** For every cancellation or size reduction at the best bid or ask, its rank in the queue,
normalized so 0 is the front and 1 the back (0.5 if every order were equally likely to cancel):

| orders at the level | mean normalized rank (95% CI over days) | share from the back half | range of the daily mean |
|---|---:|---:|---:|
| 2-4 | 0.562 [0.514, 0.607] | 57.2% [51.8%, 62.1%] | 0.419-0.683 |
| 5-9 | 0.676 [0.641, 0.714] | 71.6% [67.7%, 75.9%] | 0.600-0.831 |
| 10 or more | 0.725 [0.690, 0.766] | 76.5% [72.8%, 80.6%] | 0.625-0.873 |

Short queues are close to uniform (and on some days front-weighted); in longer queues cancellations come
disproportionately from the back, on every one of the twelve days. Orders at the back
are the newest, so this is consistent with fast participants posting and pulling quotes while older orders keep their
priority. A proportional ("uniform") cancellation model therefore moves a simulated order up the queue too fast.

**Which model predicts fills.** Every 5 seconds a zero-size probe joins the back of the best bid and of the best ask. Its
true fill time is exact: the first trade at its price that reaches an order which joined after it (or a trade through its
price). Six estimators see only what a level-2 observer sees and predict the same fill. They differ in the share of an
unexplained level decrease attributed to the queue ahead: front (all of it), back (none unless it must), proportional
(a/(a+b)), power 2 and 3 (a^n/(a^n+b^n), as in hftbacktest), and logarithmic. Scored on queue-sensitive probes (no trade
through the price, at least two orders at the level when joining), with intervals over days:

| rule | predicted / true fills | predicted too early | predicted too late | mean timing error |
|---|---:|---:|---:|---:|
| front | 1.033 [1.02, 1.04] | 30.3% [24.9%, 35.6%] | 0.0% | 4.50 s [2.45, 7.04] |
| proportional | 1.007 [1.00, 1.01] | 15.1% [11.8%, 18.7%] | 9.8% | 2.78 s [1.70, 4.08] |
| power 2 | 0.999 [1.00, 1.00] | 8.3% [6.5%, 10.3%] | 12.3% | 2.21 s [1.54, 2.99] |
| **power 3** | **0.998 [1.00, 1.00]** | **7.6% [5.9%, 9.5%]** | 11.7% | **2.11 s [1.48, 2.84]** |
| logarithmic | 1.022 [1.01, 1.03] | 23.5% [18.9%, 28.2%] | 10.3% | 4.16 s [2.47, 6.24] |
| back | 0.993 [0.99, 0.99] | 0.0% | 20.3% | 2.16 s [1.62, 2.75] |

414,206 probes; 153,818 queue-sensitive, of which 85.8% truly filled within the 300 s horizon. Early and late are shares
of probes filled both in truth and by the rule; intervals are 95% bootstraps over days.

![Six queue models against level-3 truth](img/queue_rules_vs_truth.png)

- **Every rule gets the number of fills nearly right** (within 3.3%); the fill count is not where queue models differ.
  They differ in *when*: a rule that fills too early is an optimistic backtest, one that fills too late a pessimistic one.
- **The proportional rule is optimistic**, as the cancellation measurement predicts: it moves an order up the queue as
  fast as cancellations shrink the level, but most cancellations come from behind. It predicts 15.1% of fills too early,
  twice the power-3 rule's 7.6%, and the intervals do not overlap.
- **Power 3 is the best single rule** on early fills and mean timing error; power 2 is statistically indistinguishable
  from it. The bounds behave as bounds: "front" is always early, "back" never.
- Timing errors are small in absolute terms (a median of zero for every rule: most fills happen at a moment every rule
  agrees on). The differences matter where a backtest's fills are marginal, which is exactly quoting at the touch.

**Consequence for part 1.** The market-making simulator's cancellation rule is now a parameter (`--cancel-power`), and
the grid includes the power-3 rule next to the proportional one:

| strategy (10 ms, no fees) | BTC fills, prop -> pow3 | BTC total PnL (USDT) | ETH 25 min fills | ETH total PnL (USDT) |
|---|---:|---:|---:|---:|
| touch | 1,246 -> 1,097 | -51.54 -> -51.03 | 1,438 -> 1,387 | -3.01 -> -3.11 |
| fixed h=2 | 512 -> 506 | -44.51 -> -44.51 | 1,439 -> 1,432 | -3.53 -> -3.53 |
| fixed h=10 | 297 -> 297 | -45.08 -> -45.08 | 914 -> 906 | -2.48 -> -2.48 |
| fixed, A-S width | 201 -> 201 | -26.72 -> -26.72 | 192 -> 182 | -1.75 -> -1.75 |
| A-S gamma/k=4e-2 | 164 -> 172 | -18.19 -> -18.19 | 95 -> 92 | -0.28 -> -0.28 |
| A-S gamma/k=4e-1 | 66 -> 76 | -6.96 -> -5.41 | 27 -> 26 | -0.09 -> -0.09 |

The more realistic rule changes *how many* fills quoting at the touch gets (12% fewer on BTC, 4% fewer on ETH, because
the queue ahead now shrinks more slowly) but not the conclusion: every strategy still loses, by about the same
basis points of notional. Part 1's findings do not rest on the proportional assumption. All cells are in
`results/*/grid.csv` and `results/*/cancel_rule_*.csv`.

# Part 3: order-book signals (Coinbase L3)

**Question.** Is there short-horizon information in the book, how much, and is it enough to trade on? Everything is fitted on
some days and scored on a held-out day (leave one day out), so no number below is in-sample.

**Features** (`exsim_features`, one-second bars of exchange time, book state at the bar's end): queue imbalance at the best
quotes, I = (bid size - ask size) / (bid size + ask size); the same over the top five levels; order-flow imbalance (Cont,
Kukanov and Stoikov 2014) over the last bar and the last 10 seconds, scaled by average depth; and signed trade volume over
the last bar. Bars touching a gap in the archive are excluded, along with any target that spans one.

**1. Queue imbalance and the next mid move** (Gould and Bonart 2016). Target: is the next change of the mid price up?

| feature | AUC on the held-out day (95% CI over days) | accuracy | majority-class baseline | worst day AUC |
|---|---:|---:|---:|---:|
| queue imbalance, best quotes | 0.687 [0.677, 0.695] | 63.4% [62.6%, 64.3%] | 53.9% | 0.652 |
| depth imbalance, top 5 levels | 0.641 [0.631, 0.650] | 59.8% [59.2%, 60.5%] | 53.9% | 0.607 |

The relation is monotone and stable across days: when the bid queue is nearly empty relative to the ask (I below -0.8)
the next move is up 33.7% of the time; when the ask queue is nearly empty (I above 0.8), 77.0%. The best level carries
more information than the top five, as Gould and Bonart found for large-tick stocks; BTC-USD at a $0.01 tick is a very
large-tick instrument in that sense (the spread is almost always one tick). 1.04 million valid one-second bars.

**2. Contemporaneous price impact of order flow.** Cont, Kukanov and Stoikov find mid-price changes are close to linear in
order-flow imbalance over the same interval. Per day, R^2 of that regression:

| interval | R^2 (95% CI over days) | range over days |
|---|---:|---:|
| 1 s | 0.324 [0.275, 0.376] | 0.225-0.499 |
| 10 s | 0.310 [0.246, 0.376] | 0.128-0.521 |
| 60 s | 0.235 [0.176, 0.293] | 0.082-0.413 |

This replicates the literature on a different venue and asset; it is not a trading signal, since both sides are measured
over the same interval.

**3. Predicting returns.** OLS of the mid return over the next 1, 10 and 60 seconds on each feature (15 tests), scored by
out-of-sample R^2 against a zero forecast on each held-out day; a one-sided test across days, Holm-corrected over all 15:

| feature @ horizon | OOS R^2 (95% CI over days) | days > 0 | p | Holm p |
|---|---:|---:|---:|---:|
| order-flow imbalance, last 1 s @ 1 s | 0.0060 [0.0041, 0.0080] | 12/12 | 7.3e-05 | **0.001** |
| order-flow imbalance, last 10 s @ 1 s | 0.0014 [0.0007, 0.0020] | 11/12 | 8.6e-04 | **0.008** |
| queue imbalance @ 10 s | 0.0106 [0.0070, 0.0150] | 12/12 | 2.4e-04 | **0.002** |
| depth imbalance @ 10 s | 0.0079 [0.0053, 0.0102] | 11/12 | 5.2e-05 | **0.001** |
| order-flow imbalance, last 1 s @ 10 s | 0.0021 [0.0015, 0.0029] | 12/12 | 6.8e-05 | **0.001** |
| queue imbalance @ 60 s | 0.0033 [0.0021, 0.0046] | 12/12 | 1.6e-04 | **0.002** |
| depth imbalance @ 60 s | 0.0027 [0.0018, 0.0037] | 12/12 | 1.6e-04 | **0.002** |
| order-flow imbalance, last 10 s @ 10 s | 0.0008 [0.0003, 0.0014] | 9/12 | 0.010 | 0.08 |
| trade imbalance @ 10 s | 0.0003 [0.0000, 0.0005] | 9/12 | 0.027 | 0.19 |
| order-flow imbalance, last 1 s @ 60 s | 0.0001 [0.0000, 0.0003] | 9/12 | 0.044 | 0.27 |
| queue imbalance @ 1 s | 0.0044 [-0.0180, 0.0227] | 9/12 | 0.35 | 1 |
| depth imbalance @ 1 s | 0.0023 [-0.0156, 0.0163] | 9/12 | 0.40 | 1 |
| trade imbalance @ 1 s | 0.0002 [-0.0015, 0.0016] | 9/12 | 0.43 | 1 |
| trade imbalance @ 60 s | -0.0001 [-0.0005, 0.0001] | 6/12 | 0.83 | 1 |
| order-flow imbalance, last 10 s @ 60 s | -0.0001 [-0.0003, 0.0001] | 5/12 | 0.88 | 1 |

- **Seven of fifteen survive the correction**, each positive on 11 or 12 of 12 held-out days: the book does predict
  short-horizon returns. Uncorrected, ten would have looked significant at 5%; three of those were not robust.
- **It predicts very little.** The best, queue imbalance at 10 seconds, explains 1.1% of the variance of 10-second returns
  out of sample. With 10-second returns having a standard deviation of 2.46 bp, that is a predictable component of about
  0.25 bp (the square root of 1.1% times 2.46).
- **Too little to trade on as a taker.** Crossing BTC-USD's spread is nearly free (one cent on a price in the tens of
  thousands, a median of 0.0013 bp), so the binding cost is the exchange fee, which on Coinbase is quoted in basis points, not hundredths of one.
  The use of this information is passive: skewing or pulling quotes when the queue on one side is about to empty, which is
  where part 1's adverse selection comes from.
- **At one second, queue imbalance is erratic out of sample** (a positive mean but an interval spanning zero): a few days'
  extreme returns dominate the squared errors. Its directional signal at one second is strong (question 1); the size of the
  move it predicts is not stable.

![Queue imbalance and predictability](img/signals_coinbase.png)

## What parts 2 and 3 do not show

- **One product, one venue.** BTC-USD on Coinbase has a tick of $0.01 on a price between roughly $60,000 and $120,000 over
  these days (about 0.001 bp), so its spread is almost always one tick and the queue at the touch is what matters. Assets
  whose spread spans many ticks behave differently.
- **Twelve days, one per month.** They span a year and very different price levels, which is a strength for out-of-sample
  testing, but they are not consecutive, so nothing here speaks to day-to-day persistence.
- **Queue probes are zero-size and cannot be adversely selected by their own presence.** A real order of meaningful size
  changes the queue it joins.
- **Signals are measured at one-second resolution.** Faster horizons, where queue imbalance is typically strongest, are left
  out deliberately: the conclusions would then depend on a latency model.

## Reproducing parts 2 and 3

```bash
# after fetching and converting days as in data/README.md
for f in data/l3/*.exl3; do d=$(basename $f .exl3)
  build/release/exsim_queuestudy --in $f --out-prefix results/queue/$d > results/queue/$d.txt
  build/release/exsim_features   --in $f --out-prefix results/signals/$d
done
python scripts/analyze_queue.py results/queue/*.probes.csv --img docs/img --out results/queue/summary.json
python scripts/analyze_signals.py results/signals --img docs/img --out results/signals/summary.json
```
