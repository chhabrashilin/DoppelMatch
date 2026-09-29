# Benchmarks

Every number here comes from the code in this repository and can be regenerated with
`scripts/bench.sh` and `scripts/cache_profile.sh`. Design choices that did not pay off are reported
with their numbers, not dropped.

## Environment and its limits

| | |
|---|---|
| CPU | Intel Core i7-1355U (2P + 8E cores, 15 W laptop part), 48 KiB L1d / 1.25 MiB L2 per P-core, 12 MiB L3 |
| OS | Ubuntu 26.04 under **WSL2** (Hyper-V VM) on Windows 11, kernel 6.6 |
| Compiler | GCC 16.2, `-O3 -march=native`, C++20 |
| Tuning | Threads pinned with `pthread_setaffinity_np`. **No** isolcpus, no fixed clocks, turbo on, on battery/AC as found. |

This is a poor machine to benchmark on, and I measured how poor. `exsim_pipeline --jitter 10` spins a
pinned thread on back-to-back `rdtsc` reads:

```
jitter probe: 10.0 s on cpu 4 (pinned: yes)
  TSC read-to-read gap  n=755103595  p50 10  p90 17  p99 31  p99.9 48  p99.99 220  max 10833984 ns
  time lost to gaps > 1 us: 12.250%; gaps > 10 us: 14533 (1453.3 per second)
```

The hypervisor takes the core away about 1,450 times a second, for up to 10.8 ms at a time. That was
the quieter of the probes taken during this project:

| When | Time lost to gaps > 1 us | Gaps > 10 us per second |
|---|---:|---:|
| Campaign 1 (first benchmark run) | 12.3% | 1,453 |
| Gateway and study sessions | 6.3% | 1,291 |
| Campaign 2 (final benchmark run) | **37.2%** | 1,300 |

This has two consequences:

1. **Wall-clock throughput drifts between runs** (thermal and turbo budget, host load), and it did:
   `aos` best-of-15 was 26.3 M msg/s in campaign 1 and 17.9 M msg/s in campaign 2, with the baseline
   moving the same way (9.7 to 5.9). Repetitions are therefore interleaved round-robin across
   implementations, best-of-N is the headline (noise only ever adds time), the median is reported
   alongside, and speedups are the median of **paired** repetitions with a bootstrap interval. Ratios
   were stable across campaigns (2.7x and 3.1x); absolute numbers move by ~30%.
2. **Tail latency above roughly p99 measures the hypervisor, not the code.** Per-message engine
   percentiles are reported up to p99.9; pipeline tails are reported but attributed.

For cache behaviour, wall-clock is too noisy to settle small layout questions, so the deterministic
instrument is **cachegrind**: simulated 48 KiB D1 / 12 MiB LL. It has no L2, no TLB and no hardware
prefetcher, and only the timed matching loop is instrumented, via client requests. Its counts are
exact and repeatable, but they are a model, not silicon. On bare-metal Linux,
`scripts/cache_profile.sh` uses `perf stat` hardware counters instead.

## Workload

`generate_workload()` produces a deterministic stream (seed 42) across 8 symbols:
63.6% new orders (6.2% marketable, 3.8% IOC/FOK/market, ~3% post-only), 31.4% cancels (1% of them
for orders that already filled, as when a cancel races a fill), and 5.0% modifies (half in-place
size reductions, half reprices). Resting interest sits a geometric distance from a random-walking
mid, and each book holds about 4,096 live orders in steady state. The 5M-message benchmark stream
produces 1.46M trades.

Each book is configured for a 65,536-tick price band and **262,144 order capacity**, sized for peak
load rather than the steady state. That gap is what made design point 2 matter.

## Results by design point

Every design point below is still compiled into `exsim_bench` and runs against the identical stream.
All five produce **byte-identical event streams** (digest `b5661fe697165fae`), checked on every
benchmark run.

**Throughput**: 5M messages, 15 interleaved reps, pinned to one core:

| # | Design point | M msg/s best | M msg/s median | ns/msg (best) | vs baseline | heap allocs in loop |
|---|---|---:|---:|---:|---:|---:|
| 0 | `ref`: `std::map<price, std::list<Order>>` + `std::unordered_map` | 9.68 | 7.61 | 103.3 | 1.00x | 4,934,143 |
| 1 | `aos-scatter`: dense ladder + bitmap + slab pool + flat index (fmix64) | 11.41 | 8.52 | 87.6 | 1.18x | 0 |
| 2 | `aos`: same + **locality-preserving index hash** (shipped default) | **26.31** | **15.75** | **38.0** | **2.72x** | 0 |
| 3 | `soa`: same, pure structure-of-arrays order store | 24.11 | 16.86 | 41.5 | 2.49x | 0 |
| 4 | `hybrid`: same, 32 B hot record + cold audit array | 25.17 | 17.83 | 39.7 | 2.60x | 0 |

**Per-message latency** (serialized `lfence; rdtsc … rdtscp; lfence` around `engine.process`; the
timer's own ~17 ns is not subtracted; 5 interleaved passes):

| Design point | p50 | p90 | p99 | p99.9 | mean |
|---|---:|---:|---:|---:|---:|
| `ref` | 119 ns | 300 ns | 1,030 ns | 3,923 ns | 190 ns |
| `aos-scatter` | 177 ns | 380 ns | 760 ns | 3,040 ns | 235 ns |
| **`aos`** | **52 ns** | **184 ns** | **564 ns** | **2,305 ns** | **103 ns** |
| `soa` | 53 ns | 174 ns | 478 ns | 2,010 ns | 98 ns |
| `hybrid` | 50 ns | 187 ns | 588 ns | 1,961 ns | 106 ns |

By message type (`aos`): new order p50 **37 ns** / p99 429 ns; cancel p50 125 ns / p99 711 ns;
modify p50 125 ns / p99 735 ns.

**Cache and branch profile** (cachegrind, 1M messages, timed loop only):

| Design point | instructions | D refs | D1 misses | LL misses | branch mispredicts |
|---|---:|---:|---:|---:|---:|
| `ref` | 440.0 M | 217.9 M | 9.19 M | 0.93 M | 7.16 M |
| `aos-scatter` | 176.8 M | 78.0 M | 4.23 M | 1.65 M | 1.18 M |
| **`aos`** | **161.0 M** | **79.7 M** | **3.99 M** | **1.48 M** | **1.19 M** |
| `soa` | 163.9 M | 88.1 M | 8.35 M | 5.22 M | 1.23 M |
| `hybrid` | 159.6 M | 81.6 M | 4.17 M | 1.76 M | 1.24 M |

### 0 → 1: the data structures

The textbook engine pays for its generality everywhere. Every new price level is a red-black tree
node allocation, and every resting order is a `std::list` node plus an `unordered_map` node.
That is 4.9M heap allocations in 5M messages. Finding the next price walks tree pointers.

Design point 1 replaces all of it:

- a **dense price ladder** (level = array index; 16-byte levels, four per line)
- a **two-level bitmap** to find the next best price in O(1) word operations
- a **slab pool** with an embedded LIFO free list
- an **open-addressing id index** with backward-shift deletion

Branch-free crossing checks replace emptiness tests: the empty-book sentinels are -1 and
num_levels.

The result is **−60% instructions, −54% D1 misses, −84% branch mispredicts, zero allocations**.
Yet throughput improved only 18%, and median latency got *worse*. The LL misses explain why: they
went **up** 1.8x.

### 1 → 2: the finding that mattered most

**Problem.** The id index is sized for capacity: 2^19 slots. The textbook hash (Murmur3 fmix64)
scatters ids uniformly over it. The ~4K orders live in a book at any moment are therefore spread
across the whole table, and nearly every lookup (dup check, cancel, fill, modify) touches a cold
cache line and usually a cold page. The `std::map` baseline, whose containers grow only to the
live set, had the smaller working set.

**Fix.** A **locality-preserving hash**: keep the low bits of the id, folded with the bits above
the table size. Ids are issued roughly sequentially, so the live window stays compact and
cache-resident whatever the capacity.

**Result.** **2.3x throughput** (11.4 → 26.3 M msg/s) and **3.4x lower median latency**
(177 → 52 ns). Only −6% D1 / −10% LL in cachegrind. The rest of the win is in the L2 and the dTLB,
which cachegrind does not model. That is exactly why the wall-clock measurement matters.

**Confirming it's footprint, not code.** The capacity sweep below shows the same pattern. At a
capacity small enough for the whole table to fit in L2, scatter and locality converge:

| capacity / book | `ref` | `aos-scatter` | `aos` | `soa` | `hybrid` |
|---:|---:|---:|---:|---:|---:|
| 262,144 | 9.97 | 13.79 | **29.63** | 24.89 | 28.43 |
| 16,384 | 9.49 | 38.33 | **43.73** | 35.63 | 41.38 |

(M msg/s best-of-9, 3M messages.)

**Trade-off.** A client who picks ids strided by the table size can build long
probe chains under `LocalityHash`. That is a latency-DoS vector if ids are client-chosen and
untrusted. Real venues assign order ids themselves, which removes it. Otherwise use `ScatterHash`
(a template argument) or salt the fold. `order_index_locality_survives_strided_keys` tests that the
worst case stays correct.

**Follow-on: 8-byte tagged index slots.** A line-level cachegrind profile (`-O3 -g`, `cg_annotate`)
attributed about **40% of all D1 misses and 40% of LL misses to the id index**. Index slots were
then cut from 16 bytes `{u64 key, u32 slot}` to 8 bytes `{u32 fingerprint, u32 slot}`, with
matches verified against the order record, which every operation touches anyway. Measured on
`aos`: D1 misses 4.16M → 3.99M (−4%), LL misses 1.67M → 1.48M (−11%).

### 2 → 3 / 4: the structure-of-arrays hypothesis, refuted

The premise was that a structure-of-arrays order store is more cache-friendly. **For this workload
it is not.**

**Pure SoA doubles D1 misses (2.1x) and multiplies LL misses by 3.5x.** Matching, cancel and modify each access
*one order* and touch most of its fields: id, qty, next, prev, level, side, owner. In pure SoA
those fields live in nine different arrays, so one order costs up to nine cache lines instead of
one. SoA wins when a loop streams one field across many elements. An order book's access pattern
is the opposite: random access to whole records.

**The hot/cold split was measured twice:**

| Variant | D1 misses vs AoS | LL misses vs AoS | Verdict |
|---|---:|---:|---|
| 16 B `{id, qty, next}` "matching" record + 16 B `{prev, level, owner, side}` "link" record + audit | +20% | +59% | rejected: cancel/modify/insert need both halves |
| 32 B record of every engine-touched field + 16 B cold audit `{ts, orig_qty}` (shipped as `hybrid`) | +5% | +19% | read misses −7%, but the separate audit writes cost more than that saves |

**Wall-clock does not separate the three layouts.** Their bootstrap intervals overlap in both
campaigns (campaign 2, speedup over `ref`: `aos` 3.11x [2.18, 3.85], `soa` 2.68x [2.18, 3.29], `hybrid`
3.41x [1.60, 3.69]), and the ordering by best-of-N flips between campaigns. The extra D1 misses in
SoA mostly hit L2, which is cheap. `aos` is the default because it has the fewest *simulated* misses,
which is deterministic; that is a weaker claim than "fastest", and it is stated as such. The other two
stay in the suite so the comparison can be rerun on any machine.

### Campaign 2: the same suite on a noisier host (37% of the pinned core lost)

5M messages, 15 interleaved reps. Absolute throughput is lower, ratios and conclusions hold.

| Design point | M msg/s best | M msg/s median | speedup over `ref` (median of paired reps, 95% CI) | p50 / p99 per message |
|---|---:|---:|---|---|
| `ref` | 5.85 | 4.16 | 1.00x | 244 ns / 1,904 ns |
| `aos-scatter` | 10.39 | 7.08 | 1.78x [1.66, 2.23] | 213 ns / 1,538 ns |
| `aos` | 17.86 | 11.59 | 3.11x [2.18, 3.85] | 106 ns / 879 ns |
| `soa` | 19.51 | 9.82 | 2.68x [2.18, 3.29] | 137 ns / 1,392 ns |
| `hybrid` | 18.22 | 10.39 | 3.41x [1.60, 3.69] | 100 ns / 1,025 ns |

The locality-hash step (`aos-scatter` to `aos`) is present in both campaigns (1.18x to 2.72x, and 1.78x
to 3.11x). Latency percentiles roughly doubled with the noise (`aos` p50 52 ns to 106 ns), which is the
reason latency is reported with the jitter probe next to it.

## Head to head with Liquibook

[Liquibook](https://github.com/enewhuis/liquibook) is an established open-source C++ matching engine
(`std::multimap` levels, callbacks flushed after each operation). `exsim_bench_liquibook` runs both
engines on the **identical** command stream, restricted to the semantics they share (Day and IOC limit
orders plus cancels).

**Correctness gate first.** Before any timing the two engines must agree on the number of trades and
the total traded volume: **933,619 trades and 96,761,274 lots, identical**, on 3M commands over 8
symbols. That is an independent cross-validation of this engine's matching.

| 15 interleaved reps | best | median | worst |
|---|---:|---:|---:|
| exsim (`aos`) | 25.90 M msg/s | 18.87 | 10.92 |
| Liquibook (`SimpleOrder`) | 1.07 M msg/s | 0.98 | 0.34 |

**Speedup: 21.9x** (median of 15 paired reps, 95% bootstrap CI 18.5x to 24.8x). An earlier 9-rep run
gave 17.6x [14.8, 18.5]. Read this as "a conventional design costs this much on this workload", not as
a like-for-like feature comparison: Liquibook also supports stop orders, depth tracking and
per-operation callbacks, all of which cost time and none of which this engine does.

## The locality hash: cost of its weakness

`exsim_bench --adversarial` measures the id index directly (2,048 live orders, capacity 262,144, ns
per lookup, cache-hot):

| key pattern | locality hash | scatter hash (fmix64) |
|---|---:|---:|
| sequential ids (what an exchange issues) | 2.6 | 4.8 |
| random 64-bit ids | 2.9 | 4.8 |
| **crafted against the locality hash's fold** | **952.6** | 3.4 |

Keys of the form `(i << bits) | (i ^ C)` all share one home slot under `k ^ (k >> bits)`, so lookups
walk a chain of 2,048 entries: a ~300x slowdown here (an earlier, quieter run measured 767 ns vs 1.5 ns,
~500x). Correctness is unaffected (`order_index_locality_hash_stays_correct_under_a_worst_case_collision_attack`).
`fmix64` is not an answer either: it is invertible, so an informed attacker can craft collisions for it
too. The defense is exchange-assigned order ids, and that is what the gateway now does
([client_ids.hpp](include/exsim/client_ids.hpp)): the engine only sees sequential ids issued by the venue,
and the one table keyed by client input uses SipHash-1-3 under a random key. Measured end to end over TCP
(`scripts/e2e_sessions.py`, server CPU time for 40,000 resting orders then 40,000 cancels):

| client ids sent | server CPU |
|---|---:|
| sequential | 0.076 s |
| crafted to collide, gateway assigns exchange ids (default) | 0.057 s |
| crafted to collide, passed to the engine (`--trust-client-ids`) | 3.375 s (59x) |

A later run on the same machine: 0.047 s, 0.050 s and 1.949 s (39x). The ratio moves with the host; the gap does not close.

## The id index: deletion needed Robin Hood order

The session test above first failed in an unexpected way: its *baseline*, with ordinary sequential ids,
used 2.25 s of CPU for 80,000 commands. Callgrind put 81% of all instructions in the id index. The cause:
40,000 resting orders with consecutive ids occupy one contiguous run of the table, and linear-probing
deletion (Knuth's Algorithm R) must scan from the deleted slot to the next *empty* slot, since any later
entry might belong earlier. Cancelling oldest-first made every cancel scan the rest of the run: O(n^2).
Sequential ids are exactly what the gateway now issues, so this was a real exposure, not an attack.

The fix keeps each run in Robin Hood order (entries sorted by home slot; on insert, the entry further from
its home keeps the slot). Deletion can then stop at the first entry sitting in its home slot, and an
absent-key lookup can stop once it has probed further than the resident entry. The probe distance is
recomputed from the stored fingerprint, so the slot stays 8 bytes. `exsim_bench_index` (a copy of the old
algorithm against the current one, capacity 2^18, ns per erase, best of 5):

| live orders | erase order | linear probing | Robin Hood | speedup |
|---:|---|---:|---:|---:|
| 1,000 | oldest first | 1,923.5 | 4.0 | 485x |
| 1,000 | random | 52.5 | 20.6 | 3x |
| 4,000 | oldest first | 4,571.3 | 4.4 | 1,032x |
| 4,000 | random | 27.6 | 10.4 | 3x |
| 40,000 | oldest first | 56,516.9 | 3.4 | 16,403x |
| 40,000 | random | 40.6 | 9.6 | 4x |

On the standard benchmark workload the change is neutral to slightly positive in the deterministic cache
simulation (1M messages, `aos`, before vs after): instructions -0.8%, data references -4.0%, D1 and LL
misses unchanged (4.16M, 1.51M), branch mispredictions -16.9% (1.47M to 1.22M). The design-point
throughput figures earlier in this file were measured before the change. A full run afterwards (with 64-bit
quantities as well, 15 interleaved reps, a quiet host) gave `aos` 27.49 M msg/s best and 23.21 median, 2.60x over
the `std::map` reference (paired median, 95% CI 2.16x to 2.81x): consistent with both earlier campaigns.
The differential and model tests (`order_index_*`) now also assert the Robin Hood invariant after every
operation.

## 64-bit quantities

Quantities were widened from 32 to 64 bits so real Coinbase sizes (integer satoshis; 32 bits overflow at
42.9 BTC) fit exactly. Cost, from the cache simulation (1M messages, same workload, both builds with the
earlier index so that only the width differs): the AoS record grows from 48 to 56 bytes, Event from 40 to
48, and the hybrid hot record from 32 to 40.

| | AoS 32-bit | AoS 64-bit | change | Hybrid 32-bit | Hybrid 64-bit | change |
|---|---:|---:|---:|---:|---:|---:|
| instructions | 160.97M | 172.78M | +7.3% | 159.62M | 175.85M | +10.2% |
| data references | 79.72M | 87.02M | +9.2% | 81.56M | 88.87M | +9.0% |
| D1 misses | 3.99M | 4.16M | +4.3% | 4.17M | 4.49M | +7.7% |
| LL data misses | 1.48M | 1.51M | +1.8% | 1.76M | 1.78M | +1.2% |

A real cost, mostly in instructions and L1 traffic rather than in the last-level misses that dominated
the earlier design decisions; wall-clock differences on this host were inside run-to-run noise. Correctness
on real data is not negotiable, so 64 bits it is. With the Robin Hood index as well, the current AoS build
is at 171.31M instructions and 4.15M D1 misses: +6.4% and +4.2% over the original 32-bit build.

## Gateway (TCP, loopback, WSL2)

`scripts/e2e_gateway.sh`: a client sends orders over TCP; the server decodes, journals (write-ahead),
runs the risk gate and the engine, and returns execution reports. Numbers are loopback on a virtualized
host with unpinned processes, so read them as an existence proof, not a tuned figure.

| Test | Result |
|---|---|
| Correctness, 500,000 orders, closed loop | server output digest equals a local engine's, **IDENTICAL** (653,293 events) |
| Closed-loop throughput (window 256) | 0.31 M commands/s (one syscall per command each way; not batched) |
| Round trip at 100,000 msg/s offered, open loop | p50 27.5 us, p90 0.70 ms, p99 9.6 ms (latency measured from the *scheduled* send time) |
| `kill -9` under load, then `--recover` | every acknowledged command recovered; recovered digest equals an independent offline replay |
| Journal truncated mid-record | detected, discarded, journal reusable |

The p50 is the true cost of a TCP round trip through the whole stack; the millisecond p99 is the
hypervisor and unpinned scheduling, as the jitter probe predicts.

## Replication (loopback multicast, WSL2)

What the hot backup costs (`scripts/e2e_replication.sh` for correctness; three interleaved rounds of the
same measurement for cost, on an otherwise idle machine). "Async" publishes the sequenced stream to the backup
without waiting; "wait" (`--replicate-wait`) releases a client's acknowledgement only after the backup has
acknowledged the batch, which is what makes failover lossless.

| mode | closed-loop throughput, median (range) | round trip at 50,000 msg/s open loop, p50 median (range) | mean wait for the backup per batch |
|---|---:|---:|---:|
| no replication | 0.368 M cmds/s (0.365-0.372) | 23.5 us (21.2-27.5) | - |
| async | 0.340 M cmds/s (0.317-0.382) | 45.5 us (41.6-106.7) | - |
| replicate-wait | 0.242 M cmds/s (0.198-0.360) | 182.0 us (166.3-225.9) | 76-86 us |

The price of never losing an acknowledged order is one loopback round trip to the backup per batch: about
160 us at the median on this host, most of it scheduling of three unpinned processes on a virtualized
laptop. Throughput suffers less than latency because the wait is per batch, not per command, and it is noisy
(one round of replicate-wait matched the unreplicated throughput). p99 figures (0.5 to 17 ms) are the
hypervisor again and are not compared.

| Failover test (kill -9 of the primary under load, 1% datagram loss) | Result, three runs |
|---|---|
| backup takes over (silence threshold 300 ms) | serving orders 309 to 364 ms after the kill |
| acknowledged commands lost | none (e.g. 314,081 acknowledged, 314,680 held) |
| backup journal vs dead primary's journal | exact prefix every time (after fixing the publish order; see DESIGN.md section 7) |
| promoted state vs offline replay of its journal | identical digests |

## WebAssembly

The same C++ engine, compiled with Emscripten (`-O3`, 65 KB with the module embedded), runs the browser
UI. In Node it does **7.1 to 17.0 M orders/s** on a private book (500,000 random orders) depending on the run,
below native: WebAssembly has no huge pages, no core pinning and a slower memory model. It is the same code,
so the gap is the platform, not the algorithm.

## Replay determinism

`exsim_gen` writes a 10M-message binary capture (400 MB). `exsim_replay` pushes it through each
implementation:

| book | events | trades | digest | replay time |
|---|---:|---:|---|---:|
| ref | 13,134,237 | 2,936,622 | `73aad034f17dc493` | 1.42 s |
| aos | 13,134,237 | 2,936,622 | `73aad034f17dc493` | 0.89 s (11.3 M msg/s) |
| soa | 13,134,237 | 2,936,622 | `73aad034f17dc493` | 1.03 s |
| hybrid | 13,134,237 | 2,936,622 | `73aad034f17dc493` | 1.18 s |

The threaded pipeline (below) prints the same digest. Threading, decoding and memory layout
never change results.

## SPSC ring

Two pinned threads moving the 56-byte `Command`: **42.4 M items/s** in campaign 1 and 27.0 M items/s
in campaign 2 (checksum-verified both times); the spread is host noise.

## Pipeline: feed → ring → engine → ring → market data

Three pinned threads. The engine thread also computes top-of-book after every message, publishes
BBO changes and trades (4.8M market-data messages per 10M inputs), and stamps a latency sample per
message.

**Saturated** (feed unthrottled): **4.71 / 4.98 / 5.08 M msg/s** over three runs. The engine
thread is the bottleneck: the ingress ring is full throughout, and the market-data ring never
backs up. It runs below single-threaded replay because every command now arrives on a cache line
freshly written by another core. A consumer-side prefetch of already-published ring slots
(`SpscQueue::prefetch`) recovered **+11-15%**, measured as an interleaved A/B (mean 4.09 → 4.72 M
msg/s).

**Paced** (ingress stamps are *scheduled* send times, which corrects for coordinated omission):

| offered load | ingress → match p50 | p90 | p99 | ingress → md consumed p50 |
|---:|---:|---:|---:|---:|
| 0.5 M msg/s | 220 ns | 8.8 µs | 1.08 ms | 453 ns |
| 1.0 M msg/s | 208 ns | 15.3 µs | 1.05 ms | 429 ns |
| 2.0 M msg/s | 214 ns | 65.9 µs | 3.62 ms | 429 ns |

The p50 is the true cost: decode, cross-core hop, matching and bookkeeping in ~210 ns, and ~430 ns
through a second hop to the market-data consumer. The p90 and above track the jitter probe. With
the engine idle more than 90% of the time at 0.5M msg/s, a millisecond p99 can only come from the
core being taken away. On an isolated core (`isolcpus`, `nohz_full`, IRQ affinity) the tail should
collapse toward the engine's own p99.9 (~2.3 µs). That is untested here, and stated as a
hypothesis.

## Reproducing

```bash
scripts/bench_baremetal.sh                         # everything below on real hardware, machine config recorded
scripts/bench.sh                                   # design points, SPSC, replay determinism, pipeline, jitter
scripts/cache_profile.sh                           # perf counters if available, else cachegrind
build/release/exsim_pipeline --jitter 10           # how noisy is this machine right now?
build/release/exsim_bench --max-orders 16384 --impl aos-scatter,aos   # the footprint effect
build/release/exsim_bench --adversarial --messages 200000 --reps 1    # cost of the locality hash's weakness
scripts/fetch_liquibook.sh                         # then re-run cmake with the checkout enabled:
cmake -S . -B build/release -DEXSIM_LIQUIBOOK_DIR=third_party/liquibook
cmake --build build/release --target exsim_bench_liquibook && build/release/exsim_bench_liquibook
scripts/e2e_gateway.sh build/release               # gateway correctness, latency, crash recovery
build/release/exsim_bench_index                    # id-index deletion: linear probing vs Robin Hood
python3 scripts/e2e_sessions.py build/release      # server CPU with colliding client ids, with and without exchange ids
scripts/e2e_replication.sh build/release           # replication correctness, failover, and a throughput comparison
```

The 32-bit vs 64-bit quantity comparison used a build of the commit before the change (`git worktree add` it, build
it, and run `scripts/cache_profile.sh` on both); the replication cost table used the loop in `scripts/e2e_replication.sh`
section R3 plus an open-loop client run (`exsim_client --rate 50000`) against each of the three server modes.
