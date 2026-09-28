# exchange-sim

A price-time priority matching engine in C++20, the exchange around it (a sequenced gateway with a write-ahead journal,
exchange-assigned order ids, and a hot backup fed over multicast that takes over when the primary dies), a second
implementation of the matching rules in OCaml, and research on **real exchange data**: twelve full days of Coinbase's
order-by-order feed and an hour of Binance's book. Every claim below is measured, tested against an independent oracle, or
shown to be false and reported that way.

**Try it in a browser (no install): open [`ui/index.html`](ui/index.html).** It runs the *real* C++ engine compiled to
WebAssembly (65 KB), with a live order book you can trade against, and the study results as charts.

## What is demonstrated

| Claim | Evidence | Where |
|---|---|---|
| **It matches like a real exchange.** Replaying twelve full days of Coinbase BTC-USD order-by-order data (791 million messages), the engine reproduces **all 284,539,832** arrivals and modifies exactly (same makers, prices, sizes, order and resting remainder) and **all 7,503,266** trades. Every divergence met on the way was traced to a feed rule or a replay bug. | Two books side by side: one applies Coinbase's messages literally, the other is the engine given only the inputs. Periodic whole-book comparisons; fault injection proves the check can fail. | [docs/VALIDATION.md](docs/VALIDATION.md) |
| **Two independent implementations agree.** A purely functional OCaml model emits identical events to the C++ engine on 10M random commands (14.5M events, all order types and self-trade modes); a planted bug is caught on the first seed. | Expect tests (`ppx_expect`), QCheck properties, a cross-language differential test in CI. | [ocaml/](ocaml/), `scripts/ocaml_diff.sh` |
| **A replicated exchange that survives kill -9 without losing an acknowledged order.** The primary multicasts its sequenced input; a hot backup repairs every gap (with 2% of datagrams dropped on purpose), stays bit-identical, and on the primary's death takes over in ~0.4 s holding every acknowledged command. | End-to-end test with real sockets, real `kill -9`, journal-prefix and digest checks. | [DESIGN.md section 7](docs/DESIGN.md#7-replication-and-failover) |
| **Fast, allocation-free matching.** 18-26 M msg/s on one core, p50 52-106 ns / p99 564-879 ns per message, zero heap allocations on the hot path. About 3x a textbook `std::map` engine and 18-22x [Liquibook](https://github.com/enewhuis/liquibook), with identical trades. | Two benchmark campaigns with bootstrap intervals; a test counts allocations. | [BENCHMARKS.md](BENCHMARKS.md) |
| **Hash flooding is closed, and so is an O(n^2) cancel path it uncovered.** Clients never choose the engine's keys; the test for that exposed that cancelling consecutive ids oldest-first was quadratic, fixed with Robin Hood ordering (up to 16,400x on that pattern, neutral elsewhere). | Server CPU over TCP with crafted ids; a deletion benchmark against the old algorithm; cache simulation. | [BENCHMARKS.md](BENCHMARKS.md#the-id-index-deletion-needed-robin-hood-order) |
| **Research with honest answers.** Passive market making loses ~1.3-2 bps of notional to adverse selection on both BTC and ETH. L3 ground truth shows cancellations come disproportionately from the back of long queues, and which L2 queue model gets fills right. Queue imbalance predicts the next price move out of sample (AUC 0.687 on held-out days); predicting returns is statistically real but economically tiny. | Day-level out-of-sample tests, block and day bootstraps, Holm correction, an independent accounting audit. | [docs/RESEARCH.md](docs/RESEARCH.md) |

Measured on a 15 W laptop under WSL2, where a jitter probe shows the hypervisor taking 6-12% of a pinned core. Ratios within
a run are solid; absolute numbers move. The [limits](#limits) say what is *not* established.

```
                       +--------------------------- deterministic core ---------------------------+
 client --TCP--> gateway --> sequencer --> journal (write-ahead, CRC32C) --> risk gate --> engine --> reports
             client ids -> exchange ids        |                                                    (taker and maker)
                                               +--> UDP multicast --> hot backup: same core, own journal,
                                                    gaps repaired        acks before the client is acknowledged,
                                                    by retransmission    promotes itself if the primary goes silent

 Coinbase L3 (Tardis) --> .exl3 --> truth book (exchange's messages)  vs  engine (inputs only) --> VALIDATION.md
                                 --> queue-position ground truth, order-book signals             --> RESEARCH.md
 Binance L2 capture   --> .exmd --> book mirror, exact vs snapshots   --> market-making simulator --> RESEARCH.md
```

## Quick start

Requires CMake 3.20+, Ninja and GCC 13+ or Clang 17+ on Linux (the headers are portable; pinning, huge pages, the gateway
and replication assume Linux). On Windows, use WSL2; `run.ps1` drives a WSL build from PowerShell.

```bash
cmake --preset release && cmake --build --preset release
scripts/verify_all.sh                    # every offline correctness check

build/release/exsim_bench --spsc                                  # design points + SPSC ring
scripts/e2e_replication.sh build/release                          # multicast, gap repair, kill -9 failover
python3 scripts/e2e_sessions.py build/release                     # two sessions; hash flooding vs exchange ids
(cd ocaml && dune runtest) && scripts/ocaml_diff.sh build/release # OCaml model and cross-language agreement

python3 scripts/fetch_coinbase_l3.py --date 2026-09-01 --minutes 5 --out /tmp/l3
python3 scripts/l3conv.py /tmp/l3 /tmp/l3.exl3 && build/release/exsim_l3replay --in /tmp/l3.exl3   # engine vs Coinbase
```

Full days of Coinbase data are 1.5-4.5 GB each and are not committed; [data/README.md](data/README.md) shows how to fetch them.

## The engine, briefly

Header-only, no dependencies: [`include/exsim/`](include/exsim/). Full rationale and rejected alternatives in
[docs/DESIGN.md](docs/DESIGN.md).

- **Dense price ladder** (level = array index) with a **two-level bitmap** for next-best-price, a cached best price whose
  empty sentinels make "does it cross" one compare, and an **intrusive FIFO** per level so cancel is O(1).
- **Slab-allocated orders** with an embedded free list, prefaulted and huge-page aligned. Three interchangeable layouts
  (`AosStore`, `SoaStore`, `HybridStore`) behind one interface, chosen by measurement.
- **Flat id index**: open addressing in Robin Hood order, backward-shift deletion (no tombstones), 8-byte
  `{fingerprint, slot}` entries, a locality-preserving hash that is safe because the venue assigns the ids.
- **Order types:** limit (Day/IOC/FOK), market, post-only; cancel; modify (shrinking keeps priority, reprice or growth loses
  it and may trade); self-trade prevention in four modes chosen per order, including Coinbase's decrement-and-cancel; a
  hard capacity that cancels with `BookFull` instead of allocating. 64-bit quantities (Coinbase sizes are satoshis).
- **Deterministic**: the output is a pure function of the ordered input, which is what makes journal replay, replication,
  differential testing and the studies reproducible.

## What the measurements changed

The first "optimized" engine was **slower** than `std::map` (0.74-0.85x). Instructions were down 60% and branch misses
84%, so the problem had to be memory: the id index was sized for capacity (8 MiB) while ~4,000 orders were live, and a
well-mixed hash scattered them across all of it. A locality-preserving hash gave **2.3x**, and 8-byte index entries another
11% off last-level misses. The structure-of-arrays layout that textbooks recommend *doubled* L1 misses and multiplied
last-level misses by 3.5; it lost, and stays in the suite as a measured negative result. Later, an end-to-end test found that
the same index made oldest-first cancellation of consecutive ids quadratic; Robin Hood ordering fixed it.

## Real exchange data

**Coinbase, level 3.** `scripts/fetch_coinbase_l3.py` downloads the "full" channel from Tardis.dev (free for the first day of
each month); `scripts/l3conv.py` converts it to exact integers; `exsim_l3replay` compares the engine with the exchange episode
by episode. Getting to exact agreement meant learning the feed's real rules from the data (how crossing modifies are
reported, snapshots taken mid-stream, gaps in the archive, three kinds of self-trade prevention, and the rare order the
exchange calls filled when its matches do not add up). Each started as a divergence; [VALIDATION.md](docs/VALIDATION.md)
lists them all.

**Binance, level 2.** `scripts/capture_binance.py` records depth diffs, trades and snapshots against one local clock;
`exsim_mdreplay` rebuilds the book and validates it against the exchange's own snapshots (573/573 exact). Two things went wrong
first, both documented: applying additions before removals invents trades, and a diff stream cannot reveal unchanged levels
deeper than its seed snapshot.

## Research

[docs/RESEARCH.md](docs/RESEARCH.md), three parts:

1. **Passive market making (Binance).** With queue-aware fills, latency and fees, every strategy loses ~1.3-2 bps of what
   it trades; Avellaneda-Stoikov's edge is inventory control, not spread capture; a naive "touch means filled" backtest
   counts ~7x the fills.
2. **Queue position, measured (Coinbase L3, twelve days).** L2 backtests must guess where cancellations come from.
   Ground truth: in queues of 10 or more orders, 76.5% of cancellations come from the back half. Of six rules, the
   power-3 rule predicts a fill too early for 7.6% of filled probes, against 15.1% for the proportional rule most
   backtests use; rerunning part 1 with it barely moves the market-making results, which is itself a finding.
3. **Order-book signals (Coinbase L3, twelve days).** Queue imbalance predicts the direction of the next mid move on
   every held-out day (AUC 0.687, worst day 0.652); order-flow imbalance explains 32% of 1-second price changes
   contemporaneously; predicting future returns survives Holm correction for 7 of 15 tests but explains at most 1.1% of
   their variance, about a quarter of a basis point, small next to trading costs.

## Verification

- **Differential testing**: identical random streams to a naive `std::map` book and to every variant; events byte-identical
  after every command, full book compared every 50 commands, edge-heavy generator.
- **Cross-language**: the OCaml model and the C++ engine print events in one canonical format; `cmp` on millions of events.
- **Real-exchange oracles**: Coinbase L3 (twelve days), Binance snapshots, Liquibook.
- **Fault injection everywhere a check could be vacuous**: dropped cancels (L3), dropped level updates (L2), dropped
  datagrams (replication), planted engine and model bugs (mutation testing), truncated journals.
- **Zero-allocation hot path** is a test (a global `operator new` counter); ASan+UBSan on the suite and the end-to-end
  tests, TSan on the ring and the pipeline.
- **Durability and replication**: real `kill -9` of the server and of the primary, journal-prefix and digest checks.
- **Accounting audit**: every market-making run's PnL recomputed from raw fill logs with no shared code.
- **CI**: GCC and Clang with `-Werror`, sanitizers, real-data checks (including 5 minutes of Coinbase fetched live), the
  gateway, replication and session tests, the OCaml model, Liquibook, WebAssembly and the browser UI.

## Bugs and wrong turns

| What happened | Caught by | Outcome |
|---|---|---|
| The optimized engine was slower than `std::map`. | Benchmark, then a capacity sweep isolating footprint | Locality-preserving index hash: 2.3x. |
| Structure-of-arrays doubled cache misses. | Deterministic cache simulation | AoS is the default; SoA and both splits are kept as negative results. |
| Cancelling consecutive ids oldest-first was O(n^2). | The *baseline* of the hash-flooding test was slow; callgrind | Robin Hood order lets deletion stop early: up to 16,400x on that pattern. |
| Makers were never told they had been filled. | The first two-session test | Unsolicited fill reports; counterparties anonymous. |
| After kill -9 the backup held commands the dead primary's journal had lost. | The failover test's journal-prefix check (it had passed by timing luck) | Publish only after the journal flush: journal, replicate, acknowledge. |
| Real-data validation failed 147/573 Binance snapshots. | Diagnosing the first mismatch by level | Diff streams cannot reveal unchanged deep levels. Compare within the provable region, reseed. |
| Book batches applied additions first and invented 109 trades. | Mutation-testing the mirror | Removals-before-additions, pinned by a unit test and real data. |
| 716 Coinbase truth-book inconsistencies on modifies. | The L3 replay | A crossing modify reports its matches before the `change`; a filling one sends no `change`. |
| Ghost orders that never cancelled. | Whole-book comparison against Coinbase | My converter forgot UUIDs too early; a `done` can precede the snapshot that lists the order. |
| A "full day" of Coinbase replayed only 5 hours, twice. | Counting records | A converter crash masked by a pipe's exit status; then a gap in the archive. `pipefail`, and resynchronization from Tardis's reconnect snapshots. |
| Coinbase trades the engine could not reproduce. | Tracing each divergence | Self-trade prevention (three modes, inferred), fat-finger modifies beyond the band, and orders reported filled whose matches do not add up. |
| Exported CSV mids were quantized to a 10-tick grid. | The independent accounting audit | `setprecision(15)`. |
| My test macro sent each command three times. | A count that was 9, not 3 | Events evaluated once. |
| The rate limiter admitted a burst of 6 when set to 5. | A unit test | Off-by-one in the GCRA slack. |
| The wire format dropped the ingress timestamp, so a replay could not reproduce risk decisions. | Designing the recovery test | The journal record stores it; the CRC covers it. |
| Mutant M4 (stale FIFO tail) hung instead of failing. | Mutation testing | Cycle-safe invariant audit; bounded walks; ctest timeout. |
| Cross-core latency of 7e18 ns. | A paced pipeline run | vCPU TSC skew made a delta negative; clamped and counted. |

## Repository layout

```
include/exsim/           the engine and infrastructure (header-only)
  order_book.hpp           matching            order_store.hpp    three layouts + slab
  order_index.hpp          id index (Robin Hood) price_bitmap.hpp next-best-price
  reference_book.hpp       naive oracle        matching_engine.hpp  routing
  protocol.hpp journal.hpp wire, write-ahead log, CRC32C
  client_ids.hpp           exchange-assigned ids, SipHash        seqstream.hpp  sequenced multicast, gap repair
  risk.hpp spsc_queue.hpp  pre-trade risk, lock-free ring
  md.hpp md_mirror.hpp     Binance L2 capture + book reconstruction
  l3.hpp                   Coinbase L3 records + truth book
  mm.hpp mm_sim.hpp        queue model, strategies, simulation loop
tools/                   server, replica, client, journal, l3replay, queuestudy, features, difffeed, mdreplay, mmsim, ...
ocaml/                   the OCaml reference model, expect tests, property tests
bench/                   design-point suite, index deletion, Liquibook head-to-head
tests/                   unit, differential, allocation, journal, risk, market-making
scripts/                 capture, fetch, convert, experiments, analyses, audits, e2e tests, CI helpers
wasm/  ui/               WebAssembly build and browser explorer
results/  data/  docs/   study outputs, sample capture, write-ups and figures
```

## Related work

[nanolob](https://github.com/Hellblazer704/nanolob) (a design-point benchmark journey, differential oracle, and real Binance
replay with an Avellaneda-Stoikov study; this project's method owes it a debt),
[Liquibook](https://github.com/enewhuis/liquibook) (the head-to-head baseline),
[CppTrader](https://github.com/chronoxor/CppTrader) (a mature engine and ITCH handler),
[hftbacktest](https://github.com/nkaz001/hftbacktest) (the power-law queue models scored in RESEARCH.md part 2),
and Brian Nigito's talk "How to Build an Exchange" (the sequencer and replicated-state-machine architecture).

## Limits

- **No bare-metal numbers yet.** WSL2 hides the PMU and injects jitter; cache results are a simulator (no L2/TLB/prefetcher).
  Tails above ~p99 measure the hypervisor. `scripts/bench_baremetal.sh` produces the whole suite, with hardware counters
  and the machine's configuration recorded, on any Linux box; reports go to `results/baremetal/`.
- **Replication is one host, one backup.** Loopback multicast with injected loss, no snapshot for a replica that falls behind
  the retransmission ring, no re-replication after promotion, and no fencing against a partitioned (not dead) primary.
- **Level 3 validation is one product on one venue**, days spread over a year. Time in force, post-only and accounts are not
  in the feed; they are inferred from each order's lifecycle, and every inference is counted.
- **The market-making study is about an hour of Binance data.** Intervals are wide and are reported.
- **Durability** is tested against `kill -9`, not power loss or disk failure.
