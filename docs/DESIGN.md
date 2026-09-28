# Design notes

Why the system is built the way it is, what was tried, and what the evidence says. Numbers reference
[BENCHMARKS.md](../BENCHMARKS.md), [VALIDATION.md](VALIDATION.md) and [RESEARCH.md](RESEARCH.md).

## 1. Scope

**Goals.** A correct price-time priority matching engine whose hot path never allocates; a deterministic,
replayable event stream; an order-entry gateway that cannot lose an acknowledged command, replicated to a hot backup
that can take over; evidence that the engine matches the way a real exchange does, order by order; a market-making
study on top of it that reports what does *not* work as plainly as what does.

**Non-goals.** Multi-venue routing, auctions, derivatives, FIX, cross-machine deployment. Some are listed under
[Limits](#11-limits-and-next-steps).

## 2. Shape of the system

```
                         +---------------------- deterministic core -----------------------+
 client --TCP--> gateway --> sequencer --> journal (write-ahead) --> risk gate --> engine --> events
  (binary)     client ids -> seq, owner, ts   CRC32C, append-only      limits       per-symbol   |
               exchange ids        |                                                books        v
                                   |                                                  exec reports (taker and maker)
                                   +--> UDP multicast (sequenced commands) --> hot backup: same core, own journal
                                        gap detection, retransmission, acks        promotes itself if the primary dies
```

Everything inside the box is a **pure function of the ordered command stream**. That single property gives, for free:

- *Crash recovery.* Replay the journal into a fresh engine and you have the pre-crash state, exactly.
- *Replication.* Send the stream to another process and it has the same state; digests prove it (section 7).
- *Verification.* Two implementations fed the same stream must emit identical events: a naive C++ book, an OCaml
  model (section 9), and Coinbase itself (section 8).
- *Reproducibility.* A bug report is a journal file. A benchmark is a capture. A study is a capture.

The rules that protect that property: no wall-clock reads inside the core (the gateway stamps `ts` once, and the
journal records it); no unordered-container iteration that affects output; no floating point in matching; ids the
engine sees are assigned by the gateway before sequencing, so every replica sees the same ones.

This is the architecture Brian Nigito describes in Jane Street's "How to Build an Exchange" talk (a sequencer, a
replicated state machine, a retransmitter, passive replicas), built at the scale of one host.

## 3. The matching engine

| Concern | Choice | Alternative | Evidence |
|---|---|---|---|
| Price levels | dense array indexed by `price - min_price`; 16 B levels | `std::map` of levels | A level lookup is one address computation. Tree walks pointer-chase and allocate. |
| Next best price | two-level bitmap (`countr_zero` / `countl_zero`) | scan for non-empty | Constant-time after the best level empties; property-tested against `std::set`. |
| Empty-side test | sentinels `-1` and `num_levels`; "does it cross?" is one signed compare | `optional`/emptiness branch | Removes a branch from every match iteration. |
| FIFO per level | intrusive doubly linked list of 32-bit slot indices | `std::deque`, `std::list` | Cancel is O(1) unlink. The head's `prev` is never read, so popping the head writes one field. |
| Order storage | slab with an embedded LIFO free list, prefaulted, 2 MiB-aligned (THP) | per-order `new` | Zero allocations in the hot path (a test counts them). LIFO reuses the cache-warm slot. |
| Id lookup | open addressing, linear probing, backward-shift deletion, 8 B `{fingerprint, slot}` | `unordered_map`, tombstones | See section 4: this is where the largest single speedup came from. |
| Event delivery | `Sink` template parameter | virtual `IEventHandler` | No indirect call per event; an ignored event costs nothing. |
| Quantities | 64-bit integer lots | 32-bit | Real Coinbase sizes are integer satoshis; 32 bits overflow at 42.9 BTC and real orders are larger. Cost: +4% simulated L1 misses, wall-clock within noise (BENCHMARKS.md). |

### Semantics worth being precise about

- **Trades execute at the resting order's price**, in price then arrival order.
- **Modify.** Shrinking size at the same price keeps queue priority (it is an in-place amend). A reprice or size
  increase is cancel/replace: it goes to the back of the queue and may trade immediately. Coinbase's feed confirms both
  rules (section 8).
- **FOK** is all-or-nothing and is decided *before* any fill, using level aggregates (or a walk of the queue when
  self-trade prevention is on, because your own resting orders then provide no liquidity or cancel you).
- **Self-trade prevention** has four modes, chosen per order (the book's mode is the default): off, cancel resting,
  cancel incoming, and **decrement and cancel** (the smaller of the two orders is cancelled and the larger reduced by
  its size; both cancelled if equal). The last one exists because Coinbase uses it, and the replay of its data needed all
  three active modes (section 8).
- **Capacity is a hard limit.** When the order store is full the remainder is cancelled with `BookFull`. The engine
  never grows, never allocates, and never throws on the hot path.

## 4. What the measurements changed

The first optimized engine was **slower** than the `std::map` baseline (0.74 to 0.85x). Instruction count and branch
mispredictions had dropped by 60% and 84%, so the cause had to be memory.

Line-level cache profiling (cachegrind with debug info) attributed about 40% of all misses to the id index. It was sized
for *capacity* (2^19 slots, 8 MiB) but held only ~4,000 live orders, and a textbook scattering hash spread those orders
across the whole table: nearly every lookup touched a cold line and a cold page.

Two fixes, in order of impact:

1. **Locality-preserving hash** (`k ^ (k >> bits)`): ids are issued roughly sequentially, so live orders occupy a
   compact window of the table whatever its capacity. 2.3x throughput. The trade-off is real and measured: keys crafted
   against the fold degrade lookups 300-500x (767-953 ns vs 1.5-3 ns). This hash is only safe where the *exchange*
   assigns the order ids, and that is now how the gateway works (section 6). (Plain `fmix64` would not have been a
   fix: it is invertible, so an informed attacker can craft collisions for it too.)
2. **8-byte tagged slots**: `{u32 fingerprint, u32 slot}` instead of `{u64 key, u32 slot}`, verified against the
   order record that every operation touches anyway. Halves the index's cache footprint: -11% last-level misses.

**Refuted: structure-of-arrays.** Pure SoA *doubled* L1 misses and multiplied last-level misses by 3.5. Matching, cancel
and modify each touch most fields of one order, so SoA turns one cache line into up to nine. Two hot/cold splits were
also measured and rejected (+59% and +19% LL misses vs plain records). Wall-clock cannot separate the three layouts
(their bootstrap intervals overlap), so the default (`AosBook`) rests on the deterministic cache simulation, and the
other two stay in the suite.

Caveat that applies to all of it: cache numbers come from a simulator (no L2, TLB or prefetcher), and the machine is a
WSL2 laptop where the hypervisor steals ~6-12% of a pinned core. Ratios within a run are solid; absolute numbers move.

## 5. Concurrency

Three pinned threads joined by SPSC rings: feed (decode, stamp) -> engine -> market-data publisher.

- **Ring.** Power-of-two capacity with free-running 64-bit indices (full/empty is subtraction; no wasted slot);
  acquire/release only, no seq_cst, no CAS; each side caches the other's index so steady state touches no shared
  line; producer and consumer lines are 128 B apart because the L2 adjacent-line prefetcher pulls lines in pairs;
  consumer-side prefetch of already-published slots gave +11-15% pipeline throughput (interleaved A/B).
- **Correctness of the ring** is checked by an in-order two-thread stress test (5M items) and by ThreadSanitizer over
  the full pipeline (0 reports).
- **Latency is measured against scheduled send times** (open loop), so a slow consumer shows up as latency instead of
  silently throttling the producer. Cross-core timestamp deltas can be negative under virtualization; they are clamped
  and counted, not wrapped.

## 6. The gateway: durability and order ids

- **Write-ahead.** The gateway appends to the journal *before* the engine sees a command, and flushes it before any
  response for that batch leaves the process. A client therefore never holds an acknowledgement for something a crash
  can erase.
- **Format.** `len | crc32c | ts | owner | payload`. The wire format omits the ingress timestamp and (for cancels and
  modifies) the owner, but risk decisions depend on both, so the record stores them and the CRC covers them.
- **Failure handling.** A short final record (crash mid-write) is a *torn tail*: discarded, and recovery truncates the
  file so appends resume cleanly. A record that fails its CRC is *corruption*: reading stops there and everything after
  it is distrusted, even if it happens to parse.
- **Tested:** a real `kill -9` of the server under load, restart with `--recover`, then (1) no acknowledged command is
  missing, (2) the recovered digest equals an independent offline replay, (3) a deliberately truncated tail is
  detected (`scripts/e2e_gateway.sh`).
- **Sync policy is explicit:** `os` survives a process crash; `batch`/`every` add `fsync` for power loss. The default
  is the fast one, and the docs say so.

**Exchange-assigned order ids** (`client_ids.hpp`). Clients name orders with their own ids; the engine never sees them.
Each new order gets the next exchange id before it is sequenced, so the journal, the engine and every replica see only
sequential ids chosen by the venue: exactly the keys the locality hash is built for. The one structure keyed by client
input, `(owner, symbol, client id) -> exchange id`, is hashed with SipHash-1-3 under a random per-process key, the
standard defence against hash flooding (checked against the reference test vectors). Reports are translated back: the
owner sees its client id, and everyone else sees 0, so counterparties are anonymous. Two details keep the engine's
behaviour identical to a run on client ids, which is what lets `exsim_client` still verify its reports byte for byte:
a duplicate client id of a live order maps to that order's exchange id, so the engine itself rejects the duplicate in
its usual validation order; and the id of an order that is not live maps to 0, which is never assigned, so the engine
reports it unknown. `scripts/e2e_sessions.py` measures the effect over TCP: 20,000 colliding client ids cost what
sequential ids cost through the gateway, and many times more when passed straight to the engine
(`--trust-client-ids`, kept only to show that).

Both sides of a trade now get a report: the taker in response to its order, the maker unsolicited (sequence 0). Before
this change a maker never learned it had been filled, which a single-client test could not notice.

## 7. Replication and failover

The engine is a deterministic state machine, so replicating it means replicating its input (`seqstream.hpp`).

- **The stream.** After journaling, the primary publishes each sequenced command over UDP multicast, 25 commands per
  1472-byte datagram (an Ethernet MTU). The order within a batch is journal flush, then publish, then (with
  `--replicate-wait`) wait, then acknowledge. The failover test caught an earlier version that sent a datagram as soon
  as 25 commands accumulated, before the batch's journal flush: after `kill -9`, the backup held commands the dead
  primary's journal had lost. No acknowledged order was affected, but the backup's journal was no longer a prefix of
  the primary's, so the publisher now only stages commands until the journal is flushed. Each datagram names the sequence number of its first command. When idle, the
  primary sends heartbeats carrying the next sequence number; without them a receiver cannot tell silence from a lost
  final datagram.
- **Recovery.** A replica that sees a jump stashes what arrived early and asks the primary's retransmitter (a unicast
  control socket serving from an in-memory ring of the last 2^20 commands) for exactly the missing range, re-asking
  after 20 ms without progress. Duplicates are ignored, so a retransmission racing the original is harmless. A request
  older than the ring gets an explicit `Unavailable`, never silence.
- **Acknowledgement.** A replica acknowledges the highest sequence number it has applied and flushed to its own journal,
  and re-acknowledges every 5 ms because acks are datagrams too. With `--replicate-wait`, the primary releases a batch's
  client acknowledgements only once a replica has acknowledged the whole batch. This is what makes failover safe: the
  backup holds every command any client has seen acknowledged. If the replica stops acknowledging for 500 ms it is
  declared lost and the primary continues alone, and says so (availability over durability, explicitly).
- **Failover.** A standby replica that hears nothing (no data, no heartbeat) for 300 ms promotes itself: it opens the
  gateway on its own port with its engine, journal and sequence number as they are. Nothing is replayed. Commands the
  primary sequenced but never delivered die with it; none of them was acknowledged.
- **Tested** (`scripts/e2e_replication.sh`): replica digest equals primary digest after 500k orders; the same with 2% of
  datagrams deliberately dropped (hundreds of gaps detected and repaired); and a `kill -9` of the primary under load,
  after which the backup serves orders within about 0.4 s, holds at least every acknowledged command, holds a journal
  that is an exact prefix of the dead primary's, takes new orders, and ends in a state whose digest equals an offline
  replay of its journal.

What this does not do: one host, loopback multicast, one backup, no snapshot for a replica that falls behind the ring,
no re-replication after a promotion, and no fencing (a primary that was only partitioned, not dead, would not know it had
been replaced). Section 11 lists these.

## 8. Does the engine match like a real exchange?

Two datasets, two levels of detail.

**Binance BTCUSDT and ETHUSDT, level 2** (`md_mirror.hpp`, `exsim_mdreplay`). Exchanges publish level totals; the mirror
represents each price level as one synthetic order and rebuilds the book in the engine. Two subtleties, both found by
testing rather than by reasoning first:

1. **Removals before additions.** A 100 ms diff can move the whole book. Applying additions first makes the batch
   transiently cross and the engine generates phantom trades (109 on the 30-second sample alone when the order is
   reversed, plus 2 failed snapshots).
2. **Completeness horizon.** A diff stream only reports levels that *change*. A level deeper than the seed snapshot that
   never changes is invisible forever, so the mirror is provably complete only between the touch and the snapshot's
   deepest level. The first validation run failed 147 of 573 snapshots for this reason. The fix: compare only inside
   the provable region, and reseed from an exactly aligned snapshot once half the seeded depth is consumed. Result:
   573/573 exact, and fault injection (dropping 6 of 648,000 updates) fails 8 snapshots.

Level 2 can show the book is reconstructed correctly. It cannot show the engine *matches* correctly, because the matching
happened at the exchange.

**Coinbase BTC-USD, level 3** (`l3.hpp`, `exsim_l3replay`). Coinbase's "full" channel publishes every order event with
its order id: received, open, done, match, change. Tardis.dev archives it, free for the first day of each month. The
replay runs two books side by side: a *truth* book that applies Coinbase's messages literally, with no matching logic,
and the *engine*, which receives only the inputs (new orders, cancels, modifies) and does its own matching. Every arrival
and every price or size modify is an episode: the engine's trades (makers, prices, sizes, in order) and resting remainder
are compared with what Coinbase actually did. Periodically the whole engine book is compared with the truth book, order
by order, in queue order. Over twelve full days (791 million messages) the engine reproduced all 284,539,832 arrivals
and modifies and all 7,503,266 trades exactly; details in [VALIDATION.md](VALIDATION.md).

Getting to exact agreement meant learning the feed's rules from the data, each one a divergence first:

| Divergence | What Coinbase actually does | Fix |
|---|---|---|
| Orders missing right after the snapshot | The snapshot is taken while the stream runs; messages newer than it appear *before* it in the file | Buffer pre-snapshot messages, replay those with sequence > snapshot |
| 716 truth-book inconsistencies on modifies | A modify that crosses reports its matches *before* the `change`, whose new size is the post-trade remainder; a modify that fills completely sends no `change` at all | Hold the matches until the `change` (or the `done`) arrives |
| Ghost orders that never cancel | My converter retired an order's UUID mapping on `done`, but a `done` can precede the snapshot line that still lists the order | Retire ids only after a delay of 1M messages |
| Arrivals that cancel another order without trading | Self-trade prevention. Accounts are hidden, but an arrival that cancels (cancel resting) or decrements (decrement and cancel) a resting order reveals that both are one account's | Infer accounts in a first pass; add decrement-and-cancel to the engine, per order |
| An arrival fills at one level, then stops although the next level still crosses | Self-trade prevention, cancel newest: the next order in priority is the arrival's own | The first pass keeps a truth book; an arrival cancelled after trading, with liquidity left at its limit, is linked to that next order |
| An arrival cancels an own order, then is itself cancelled without trading | Cancel both | Replayed as cancel incoming; the resting order's own `done` removes it |
| Funds-denominated buys trade slightly short under decrement-and-cancel | Coinbase decrements a funds order's funds, which its filled size already reflects | Add back the size of the own orders it cancelled, since the engine decrements size |
| A resting order's matches counted against its arrival | (my bug) the order was modified across the spread before anything else arrived | An arrival's episode ends when the order rests |
| A day that stopped after five hours | A gap in the archived sequence (107 to 486 messages); Tardis records a new snapshot when it reconnects | Stop scoring at the gap, discard the open episode, resynchronize from the next snapshot |
| Sells at $0.01, and a resting buy that traded as a taker above its own limit | Real orders far outside any sensible band, including a fill-modify to $783,560 (10x the market), which sends no `change` | A 2^24-tick band; arrivals and fill-modifies beyond it that never rest are clamped to the edge, which matches exactly the same orders |
| A 0.03 BTC order matched for 0.02882596, then reported "filled" | The exchange's messages do not add up for a handful of orders a day (the raw feed has no gap) | The exchange's word is final: the engine drops the unmatched remainder too; each case is counted |
| Inputs the feed does not carry | Time in force, post-only, and the size of funds-denominated market orders | Inferred from each order's own lifecycle, and counted, never hidden |

Divergences are repaired locally (the affected price levels are rebuilt from the truth book; a full rebuild is the
fallback) so one unpredictable event cannot cascade, and every repair is counted. The check can fail: dropping every
1000th cancel before it reaches the engine produces divergences and failed full-book comparisons, and CI runs both the
real check and this fault injection on the first 5 minutes of a day.

## 9. A second implementation in OCaml

`ocaml/lib/exsim_ref.ml` is the matching rules again, written independently and purely functionally: an immutable book
(`Map` of price to FIFO list), `apply : t -> command -> t * event list`, and algebraic data types for commands, events,
reasons and self-trade modes. It is about 300 lines against the C++ engine's hand-tuned data structures, which is the
point: it is simple enough to check by reading.

- **Expect tests** (`ppx_expect`) pin the full event stream of hand-written scenarios in the source, so a behaviour change
  appears as a readable diff.
- **Property tests** (QCheck) check invariants after every command of random streams: never crossed, no empty levels,
  the id index agrees with the queues, no trade worse than the taker's limit, no overfill.
- **Cross-language differential testing.** `exsim_difffeed` writes a random command stream in a canonical text format
  (the same edge-heavy generator as the C++ differential test: a 128-tick band, capacity 200, four owners, every order
  type and all four self-trade modes) and prints the C++ engine's events; the OCaml binary prints its own; `cmp` must
  find them identical. 40 seeds of 250,000 commands produced 14.5 million identical events. A deliberately planted bug
  in the OCaml equal-size decrement-and-cancel rule was caught on the first seed.

## 10. Pre-trade risk

Per-order size and notional caps, a price collar around the last trade, a per-owner generic-cell-rate limiter, symbol
and global halts. Cancels are never blocked (a participant must always be able to reduce risk). Rate limiting uses the
journaled `ts`, so a replay makes every accept/reject decision identically (tested on 30,000 commands).

## 11. Limits and next steps

- **No bare-metal numbers.** WSL2 hides the PMU and the hypervisor injects jitter. Next: `isolcpus`/`nohz_full`,
  `perf stat` hardware counters, and the same grid on real silicon.
- **Replication is one host, one backup.** Loopback multicast has no real loss (it is injected), no switch and no NIC.
  Missing: snapshots for a replica that falls behind the retransmission ring, re-replication after promotion, and
  fencing against a primary that is partitioned rather than dead (a real deployment needs an arbiter or leases).
- **One matching thread per shard.** Symbols are independent, so sharding is straightforward, but no router exists.
- **Level 3 validation is Coinbase only**, one product, days spread over a year. Hidden inputs (time in force, accounts)
  are inferred, and the inference is reported rather than assumed.
- **The market-making study is one hour of level 2 data across two assets.** Intervals are wide; see RESEARCH.md for
  what is and is not established.
