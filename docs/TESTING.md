# Testing

A matching engine can be wrong in ways that look fine: a trade at the right price with the wrong maker, a level total
that drifts by one lot, a cancel that is O(n²) only when ids are consecutive. So each layer of testing below is there
because a simpler layer could miss something, and each check is shown to fail when the thing it guards is broken.

`scripts/verify_all.sh` runs every offline check in one command.

## The layers

| Layer | What it catches | Where |
|---|---|---|
| Unit and scenario tests | Hand-computed cases: every order type, modify rules, the four self-trade modes, capacity, risk limits, journal recovery | `tests/` (180 cases, 3.2M assertions) |
| Differential testing | Any difference between the optimized books and a naive `std::map` book on random streams: events byte-identical after every command, full book state compared every 50 commands | `tests/test_differential.cpp` |
| Cross-language model | Bugs the C++ reference shares with the engine by construction: a ~300-line functional OCaml model must print identical events (14.5M on 10M commands) | `ocaml/`, `scripts/ocaml_diff.sh` |
| Real exchanges | Mistakes in *my reading* of the rules, which the two tests above cannot see: Coinbase order-by-order replay, Binance snapshots, Liquibook | [VALIDATION.md](VALIDATION.md), `exsim_mdreplay`, `exsim_bench_liquibook` |
| Fuzzing | Inputs random generators rarely produce: libFuzzer with ASan and UBSan on the wire decoder, journal recovery and the engine | `fuzz/` |
| Sanitizers | Memory errors, undefined behaviour, data races | ASan + UBSan on the suite and end-to-end tests; TSan on the ring and pipeline |
| End-to-end | What unit tests cannot reach: real sockets, `kill -9`, `SIGSTOP`, dropped datagrams, torn journals | `scripts/e2e_*.sh`, `scripts/e2e_sessions.py` |
| Planted bugs | Whether all of the above would notice a realistic mistake | the table below |

## Planted bugs

A test suite that passes proves little until you see it fail. Each row below is a realistic one-line bug, applied to a
scratch copy of the source; the unit and differential suite was rebuilt and run against it. All 12 were caught.

| Planted bug | Where | What caught it |
|---|---|---|
| A taker fills the maker's whole size instead of the smaller of the two | `sweep` | 10 test cases, first `price_then_time_priority` |
| Cancel forgets to subtract the order from its level's total | `unlink` | 32 test cases, first `cancel_semantics` |
| A size increase at the same price keeps queue priority | `modify` | Only the random differential test |
| Fill-or-kill never checks available liquidity | `fillable` | 14 test cases, first `fok_is_all_or_nothing` |
| Decrement-and-cancel with equal sizes cancels only the resting order | `sweep` | 5 test cases, first `stp_decrement_cancel_equal_sizes_cancel_both` |
| Removing the tail order leaves the level's tail pointing at it | `unlink` | Segfault in `cancel_head_and_tail` on the first optimized book |
| The bitmap's summary bit is not cleared when a word empties | `PriceBitmap::clear` | 42 test cases, first `fills_at_maker_price` |
| Insertion drops the Robin Hood swap | `OrderIndex::insert` | `book_full_cancels_remainder`, then a hang the watchdog reports |
| Backward-shift deletion stops one entry early | `OrderIndex::remove_at` | Hang in `order_index_scatter_matches_unordered_map`, reported by the watchdog |
| The journal reader skips the CRC check | `journal_scan` | `journal_detects_a_flipped_bit_and_distrusts_everything_after_it` |
| The rate limiter admits one message more than its burst | `RiskGate::check` | `risk_rate_limit_allows_a_burst_then_throttles_and_recovers` |
| The price collar only checks prices above the last trade | `RiskGate::check` | `risk_price_collar_tracks_the_last_trade` |

Two rows taught me something. The modify-priority bug is caught *only* by the random differential test: no
hand-written scenario grows an order in place and then checks who trades first. And the two index bugs did not fail at
first, they hung, because a corrupted probe sequence can loop forever. That stalled the suite until the CI timeout, so
the test runner now has a per-test watchdog that turns a hang into a failure naming the test, within 60 seconds.

`scripts/mutation_check.py` applies them (each bug is listed in its source with the exact text it replaces), and CI
runs it on every push, so a change that makes the suite blind to one of these fails the build.

Beyond these, the real-data checks each have a built-in fault injection that CI runs: dropping every 1,000th cancel
before it reaches the engine must make the Coinbase replay diverge, dropping level updates must fail Binance snapshots,
and the replication and market-data tests drop 2% of datagrams on purpose and require full recovery.

## Fuzzing

Three libFuzzer targets, built with `-DEXSIM_FUZZ=ON` (Clang), each run under ASan and UBSan:

| Target | Property | Executions in 120 s |
|---|---|---:|
| `fuzz_wire` | The order-entry decoder never over-reads, and any message it accepts re-encodes to the same command | 113.7M |
| `fuzz_journal` | Structure-aware: N valid records followed by a fuzzed tail. The valid prefix always survives, and recovery leaves a clean, appendable file | 1.25M |
| `fuzz_book` | Commands decoded from the bytes run on a tiny book (64 ticks, capacity 24, three owners) so every edge is close; the production, hybrid and reference books must agree event for event and pass a structural audit | 0.94M |

No crashes or sanitizer reports. CI runs each target for 60 seconds on every push.

## Coverage

Clang source-based coverage over every suite (unit, differential, gateway, sessions, replication, market data, the
Binance replay and the market-making simulator), `scripts/coverage.sh`:

| | Lines | Branches |
|---|---:|---:|
| **All of `include/exsim/` plus the gateway** | **93.2%** | **81.3%** |
| `order_book.hpp` (matching) | 100% | 89.1% |
| `order_index.hpp` (id index) | 100% | 92.1% |
| `risk.hpp` | 100% | 83.3% |
| `mdfeed.hpp` (market data) | 98.2% | 76.6% |
| `seqstream.hpp` (replication) | 90.8% | 75.8% |
| `tools/gateway.hpp` | 91.2% | 72.7% |

Most of what is not covered is error handling for failed system calls and `cpu.hpp` (thread pinning, which only the
benchmarks use).

Coverage says code ran, not that its result was checked. The differential tests and the real-exchange replays are what
check results; coverage is how I know they reach the code.

## End-to-end

| Script | What it does to the system | What must hold |
|---|---|---|
| `e2e_gateway.sh` | 500,000 orders over TCP; `kill -9` under load; a journal truncated mid-record | Output digest equals a local engine's; recovery loses no acknowledged command; torn tail detected and trimmed |
| `e2e_sessions.py` | Two clients trading with each other; 40,000 colliding client ids | Makers get fill reports, counterparties stay anonymous; colliding ids cost nothing once the gateway assigns ids |
| `e2e_replication.sh` | 2% datagram loss; `kill -9` of the primary; `SIGSTOP` of the primary; two failovers in a row | Digests equal; every acknowledged order survives; the old primary fences itself or halts; the backup's journal is a prefix of the primary's |
| `e2e_marketdata.sh` | Lossless, 2% packet loss, and a subscriber joining halfway | The subscriber's rebuilt book has the same level-2 digest as the engine |
