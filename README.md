# DoppelMatch

A price-time priority matching engine in C++20, the exchange around it, and a test of whether it behaves like a real
one. Replaying twelve full days of Coinbase's order-by-order feed, it reproduces **every one of 7,503,266 trades
exactly**: same maker, same price, same size, same order.

The name is short for doppelgänger: the goal was an engine you cannot tell apart from a production venue by its output.
(The code's namespace is `exsim`, from the project's working name.)

**Try it without installing anything:** open [`ui/index.html`](ui/index.html). It runs the real C++ engine compiled to
WebAssembly (65 KB), with a live book you can trade against.

## Results

| | Result | How it is checked |
|---|---|---|
| **Matches a real exchange** | 12 days of Coinbase BTC-USD (791M messages): all 7,503,266 trades and all 4,809,848 order arrivals and modifies that traded reproduced exactly; 243M resting remainders and 1,576 whole-book comparisons also match | An independent "truth book" built only from Coinbase's messages; fault injection shows the check fails when the engine is wrong ([VALIDATION.md](docs/VALIDATION.md)) |
| **Fast** | 18-27M msg/s on one core, p50 52-106 ns / p99 564-879 ns per message, zero heap allocations; 2.6-3.1x a `std::map` engine and 18-22x [Liquibook](https://github.com/enewhuis/liquibook) on identical input | Interleaved reps with bootstrap intervals, a deterministic cache simulator, a test that counts allocations ([BENCHMARKS.md](BENCHMARKS.md)) |
| **Survives failures** | `kill -9` of the primary: the hot backup serves orders 0.3-0.4 s later holding every acknowledged order. A paused primary is fenced when it wakes up (no split brain). Two failovers in a row survive | End-to-end tests with real sockets, `kill -9`, `SIGSTOP`, 2% packet loss, journal-prefix and digest checks ([DESIGN.md §7](docs/DESIGN.md#7-replication-and-failover)) |
| **Two implementations agree** | A ~300-line functional OCaml model emits the same 14.5M events as the C++ engine on 10M random commands, and on the exact input of five minutes of real Coinbase trading (240K commands) | Cross-language diff in CI; expect tests and QCheck properties ([ocaml/](ocaml/)) |
| **Tests that bite** | 12 of 12 planted bugs caught; 93% line coverage of the engine and infrastructure (100% of the matching core); three libFuzzer targets; ASan, UBSan and TSan clean | [docs/TESTING.md](docs/TESTING.md) |
| **Research** | Passive market making loses 1.3-2 bps of notional to adverse selection. Cancels come from the back of long queues, so the proportional queue model most backtests use predicts 2x the early fills of the best rule. Queue imbalance predicts the next mid move out of sample (AUC 0.687), but the predictable return is about 0.25 bp | Day-level out-of-sample tests, block and day bootstraps, Holm correction, an independent PnL audit ([RESEARCH.md](docs/RESEARCH.md)) |

All timings are from a 15 W laptop under WSL2, where a jitter probe shows the hypervisor taking 6-37% of a pinned core.
Ratios within a run hold up; absolute numbers move between runs. Bare-metal numbers are the main thing missing.

## How it fits together

```
 client --TCP--> gateway --> sequencer --> journal --> risk --> engine --> execution reports + level-2 market data
                 (assigns       |        (write-ahead,                    (taker and maker)   (published after commit)
                  order ids)    |         CRC32C)
                                +--> UDP multicast --> hot backup: same engine, own journal. Repairs gaps by
                                     (epoch-tagged)    retransmission, acks before the client is acked, takes
                                                       over when the primary goes quiet, fences a stale one.
```

Everything from the sequencer to the engine is deterministic: the output is a pure function of the ordered input. That
one property is what makes crash recovery (replay the journal), replication (send the input, not the state) and all of
the differential testing work.

## The engine

Header-only, no dependencies: [`include/exsim/`](include/exsim/). Rationale and rejected alternatives are in
[DESIGN.md](docs/DESIGN.md).

- **Dense price ladder** indexed by `price - min_price`, a **two-level bitmap** to find the next best price, and an
  **intrusive FIFO** per level so cancel is O(1).
- **Slab-allocated orders**, prefaulted and huge-page aligned, with a hard capacity: a full book cancels with `BookFull`
  rather than allocating.
- **Flat id index** in Robin Hood order, with 8-byte `{fingerprint, slot}` entries and a hash that keeps sequential ids
  close together.
- **Order types:** limit (Day, IOC, FOK), market, post-only, cancel, modify (shrinking keeps priority, anything else
  re-queues), and four self-trade-prevention modes per order, including Coinbase's decrement-and-cancel.

## What the measurements taught me

**The first "optimized" engine was slower than `std::map`** (0.74-0.85x), with 60% fewer instructions and 84% fewer
branch misses, so the problem had to be memory. The id index was sized for 262,144 orders while about 4,000 were live,
and a well-mixed hash spread those 4,000 across the whole 8 MiB table: a filing cabinet with one folder in every drawer.
A hash that keeps sequential ids next to each other gave 2.3x. The structure-of-arrays layout that textbooks recommend
doubled L1 misses and lost; it stays in the benchmark suite as a measured negative result.

**A security test found a quadratic bug in ordinary input.** Closing the hash-flooding hole (the gateway now assigns
order ids) meant the engine only ever sees consecutive ids, and the test's *baseline* run was mysteriously slow:
cancelling consecutive ids oldest-first made linear-probing deletion scan the whole run, O(n²). Robin Hood ordering lets
deletion stop early: 16,400x faster on that pattern, neutral elsewhere.

**Real data knows rules the documentation doesn't.** Getting to zero divergences against Coinbase meant learning, one
divergence at a time, that crossing modifies report their matches before the `change`, that snapshots are taken while
the stream runs, that the archive has gaps, and that self-trade prevention is visible in the feed in all four of
Coinbase's modes. Two of the divergences were bugs in my replay, not the feed.

## Quick start

Needs CMake 3.20+, Ninja and GCC 13+ or Clang 17+ on Linux (on Windows use WSL2; `run.ps1` drives it from PowerShell).

```bash
cmake --preset release && cmake --build --preset release
scripts/verify_all.sh                                        # every offline correctness check, one command

build/release/exsim_bench                                    # the design-point benchmark
scripts/e2e_replication.sh build/release                     # failover, fencing, double failover
python3 scripts/fetch_coinbase_l3.py --date 2026-09-01 --minutes 5 --out /tmp/l3
python3 scripts/l3conv.py /tmp/l3 /tmp/l3.exl3 && build/release/exsim_l3replay --in /tmp/l3.exl3   # engine vs Coinbase
```

Full days of Coinbase data are 1.5-4.5 GB each and are not committed; [data/README.md](data/README.md) shows how to
fetch them.

## Where to read more

| | |
|---|---|
| [docs/DESIGN.md](docs/DESIGN.md) | Why it is built this way: data structures, durability, replication, fencing, market data |
| [BENCHMARKS.md](BENCHMARKS.md) | Every performance number, how it was measured, and what did not pay off |
| [docs/VALIDATION.md](docs/VALIDATION.md) | The Coinbase replay: method, per-day results, every feed rule learned from a divergence |
| [docs/TESTING.md](docs/TESTING.md) | Differential and cross-language testing, fuzzing, coverage, and the planted-bug table |
| [docs/RESEARCH.md](docs/RESEARCH.md) | Market making, queue position and order-book signals on real data |
| [docs/FAQ.md](docs/FAQ.md) | Short answers to the questions people ask about it |

## Bugs I shipped, and what caught them

| What happened | Caught by | Fix |
|---|---|---|
| After `kill -9`, the backup held commands the dead primary's journal had lost | The failover test's journal-prefix check (it had passed earlier by timing luck) | Publish to the backup only after the journal flush |
| Cancelling consecutive ids oldest-first was O(n²) | The *baseline* of the hash-flooding test was slow | Robin Hood deletion |
| Makers were never told they had been filled | The first two-session test | Unsolicited fill reports, anonymous counterparties |
| The journal ignored write errors, so a full disk could lose acknowledged orders | Code review | Every write is checked, the gateway halts, and a test writes to `/dev/full` |
| Applying a book diff's additions before its removals invented 109 trades | Mutation-testing the book mirror | Removals first, pinned by a test and by real data |
| A "full day" of Coinbase replayed only five hours | Counting records | A crash hidden by a pipe (`pipefail`), then an archive gap: resync from the next snapshot |

## Limits

- **No bare-metal numbers yet.** `scripts/bench_baremetal.sh` runs the whole suite with hardware counters on any Linux
  box; it has not been run on one.
- **Replication is one host and not consensus.** Loopback multicast with injected loss, one backup at a time, failure
  detected by timeout. Fencing plus a halt rule prevent split brain by giving up availability, not by quorum.
- **Validation covers one product on one venue.** Time in force, post-only and accounts are not in Coinbase's feed; they
  are inferred from each order's lifecycle, and every inference is counted.
- **The market-making study is about an hour of Binance data.** The intervals are wide, and they are reported.
- **Durability is tested against `kill -9`**, not power loss or disk failure.

## Related work

[Liquibook](https://github.com/enewhuis/liquibook) (the head-to-head baseline), [CppTrader](https://github.com/chronoxor/CppTrader),
[hftbacktest](https://github.com/nkaz001/hftbacktest) (the power-law queue models scored in RESEARCH.md),
[nanolob](https://github.com/Hellblazer704/nanolob) (a design-point benchmark and Binance replay whose method this project
borrows from), and Brian Nigito's talk "How to Build an Exchange", the source of the sequencer and replicated-state-machine
architecture.
