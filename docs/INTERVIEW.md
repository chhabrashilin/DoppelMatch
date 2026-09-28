# Questions this project should be able to answer

Each answer points at the code, test or measurement that backs it. If you cannot answer one of these from memory,
read the linked file before presenting the project. The answers include what is *not* established.

## The engine

**Walk me through a market buy arriving.**
`OrderBook::add` validates (id, qty, price band, duplicate), emits `Accepted`, then `sweep` runs: while the order has
quantity and `best_ask_ <= limit` (for a market order, `limit` is the top of the band), take the head of the best level's
FIFO, emit a `Trade` at the maker's price, decrement or release the maker, and when a level empties clear its bit and find
the next best with `PriceBitmap::find_next`. A market order's unfilled remainder is cancelled (`IocExpired`), never rested.
See `include/exsim/order_book.hpp`, `sweep`.

**Why is cancel O(1)?**
Cancel finds the slot through the id index (one probe in the common case), then unlinks it from an intrusive doubly linked
FIFO: two pointer writes. If the level empties, one bit clears. Nothing is searched or shifted.

**Why a bitmap for the next best price?**
After the best level empties, the next non-empty level can be arbitrarily far away (the BTC book is sparse: gaps of hundreds
of ticks). A two-level bitmap finds it with at most two word scans plus a scan of a 16-word summary, independent of the gap.
`bitmap_matches_std_set` checks it against `std::set` over 50,000 random operations per size.

**Your first optimized version was slower than `std::map`. Why?**
It had 60% fewer instructions and 84% fewer branch mispredictions, so the problem was memory. The id index was sized for
capacity (8 MiB) but held ~4,000 live orders, and a well-mixed hash scattered them across all of it, so nearly every lookup
touched a cold cache line and page. A hash that keeps live sequential ids near each other gave 2.3x. See DESIGN.md section 4.

**That hash sounds attackable.**
It was. Keys crafted against the `k ^ (k >> bits)` fold all land in one home slot and lookups go from ~1.5-3 ns to ~770-950 ns
(300-500x, `exsim_bench --adversarial`). `fmix64` would not have fixed it: it is invertible, so an informed attacker can still
craft collisions. The fix is what real venues do: clients never choose the engine's keys. The gateway assigns sequential
exchange ids before sequencing (`client_ids.hpp`), and the only table keyed by client input uses SipHash-1-3 under a random
per-process key. Over TCP, 40,000 colliding client ids cost the same server CPU as sequential ones (0.057 s vs 0.076 s), and
39 to 59 times more across runs when passed straight to the engine (`scripts/e2e_sessions.py`, `--trust-client-ids`).

**How do reports work if the engine never sees client ids?**
Every event is translated back per session: an order's owner sees its own client id and anyone else sees 0, so counterparties
are anonymous. A duplicate client id of a live order maps to that order's exchange id, so the engine rejects the duplicate in
its normal validation order; an id that is not live maps to 0, which is never assigned, so the engine reports it unknown. A
single client's report stream is therefore byte-identical to a local engine run on its own ids, which the client verifies.
Makers now get an unsolicited fill report too; before this change they got nothing, which a one-client test cannot notice.

**Tell me about a performance bug you found.**
The hash-flooding test's *baseline* was slow: 40,000 resting orders with ordinary sequential ids, cancelled oldest first, spent
81% of all instructions in the id index (callgrind). Consecutive ids fill one contiguous run of the table, and linear-probing
deletion has to scan to the next empty slot because a later entry might belong earlier, so every cancel scanned the rest of
the run: O(n^2). Sequential ids are exactly what the gateway had just started issuing. Robin Hood insertion keeps each run
sorted by home slot, so deletion can stop at the first entry sitting in its home slot. Oldest-first erase: 56,517 ns to 3.4 ns
at 40,000 live orders (`exsim_bench_index`); the standard workload is unchanged or better in the cache simulation (branch
mispredictions -17%).

**Why did structure-of-arrays lose?**
Every operation touches most fields of *one* order, so SoA turns one cache line into up to nine. Cachegrind: 2.1x the L1
misses and 3.5x the last-level misses of the plain record layout. SoA wins when a loop streams one field across many
elements; an order book is random access to whole records.

**Which layout is fastest?**
Not established by wall-clock: `aos`, `soa` and `hybrid` have overlapping bootstrap intervals (about 2.6x, 2.5x, 2.8x over the
baseline). `aos` is the default because it has the fewest simulated misses, which is deterministic; that is a weaker claim than
"fastest", and the docs say so.

## Correctness

**How do you know the engine is right?**
Six independent lines of evidence:
1. Differential testing: identical random streams to a naive `std::map` book and to every optimized variant; events must be
   byte-identical after every command, and full book state is compared every 50 commands (all four self-trade modes).
2. A second implementation in another language: a purely functional OCaml model (`ocaml/`), with expect tests and QCheck
   properties, emits identical events to the C++ engine on 10M random commands (14.5M events).
3. Mutation testing: planted bugs are caught, in the C++ (four by hand) and in the OCaml model (a wrong equal-size
   decrement-and-cancel rule, caught on the first seed of the cross-language diff).
4. Cross-check against Liquibook, an independent engine: 933,619 trades and 96,761,274 lots, identical, on 3M commands.
5. Coinbase itself: replaying twelve full days of Coinbase's order-by-order feed (791M messages), the engine reproduces all
   284,539,832 arrivals and modifies and all 7,503,266 trades exactly ([VALIDATION.md](VALIDATION.md)).
6. The reconstructed Binance book equals the exchange's own snapshots (573/573, 458,400 levels).
Each real-data check is shown to be able to fail by fault injection.

**What are the tests *not* covering?**
The C++ reference book and the OCaml model are both mine, so they share my reading of the rules; agreement between them rules
out implementation bugs, not specification mistakes. Coinbase is the independent specification, but only for the features
Coinbase has and exposes: time in force, post-only and accounts are not in its feed and are inferred, and the inference is
counted. Liquibook covers Day/IOC limit orders and cancels only.

**What did Coinbase's data teach you that the docs did not?**
That a modify which crosses publishes its matches before the `change`, whose new size is the post-trade remainder, and that a
modify which fills completely sends no `change` at all; that the snapshot is taken while the stream runs, so newer messages
precede it in the file; that the archive has gaps and Tardis records a fresh snapshot on reconnect; that self-trade prevention
is visible if you look for it, in all four of Coinbase's modes (a cancel or decrement of another order inside an arrival's
block; an arrival that stops with liquidity still in front of it after trading, which is cancel-newest; an arrival cancelled
together with its own resting order, which is cancel-both); that decrement-and-cancel reduces a funds order's funds, not its
size; and that the exchange occasionally reports an order "filled" when its published matches do not add up (41 orders in
twelve days). Each of these started as a divergence, and two of the divergences were bugs in my replay, not the feed.

**How do you know you did not just tune the replay until it agreed?**
Three things. Every rule is a statement about Coinbase's documented or observable behaviour, checked against the raw JSON,
never a per-day or per-order exception; one binary produced all twelve days. The inputs the rules supply (time in force,
accounts, funds sizes) are only ever inferred from the order's own messages, never from the engine's prediction. And the
check demonstrably fails when the engine is wrong: dropping every 1,000th cancel produces divergences and failed whole-book
comparisons within five minutes of data, which CI runs on every push.

## Concurrency and durability

**What does the SPSC ring guarantee and why is acquire/release enough?**
One producer, one consumer. The producer's release-store of `tail_` publishes the slot write; the consumer's acquire-load
of `tail_` sees it. The consumer's release-store of `head_` frees the slot for reuse; the producer's acquire-load sees that.
No total order across threads is needed, so no `seq_cst`. TSan over the full three-thread pipeline reports nothing, and a
5M-item ordered stress test passes.

**Why 128 bytes between head and tail, not 64?**
Intel's L2 spatial prefetcher fetches adjacent 64-byte lines in pairs, so data written by two threads within 128 bytes can
still ping-pong.

**What does your journal guarantee after a crash?**
Write-ahead plus flush-before-ack: any command a client saw acknowledged is in the journal. Torn tails are detected and
discarded; corrupt records stop the read and distrust everything after. `--sync os` survives a process crash, not power
loss; `batch`/`every` add `fsync`. I tested a real `kill -9` under load and compared the recovered digest with an independent
offline replay. I did **not** test power loss or disk failure.

## Replication and failover

**Why replicate the input rather than the state?**
The engine is a deterministic function of its ordered input, so the sequenced command stream *is* the state, compressed. Every
replica that applies the same commands reaches the same book, and event digests prove it. Shipping state would need
snapshots and a consistency protocol; shipping input needs only ordering and gap repair. This is the design in Brian Nigito's
"How to Build an Exchange" talk.

**UDP loses packets. How does the backup know it has everything?**
Every datagram names the sequence number of its first command, so a jump is a known range. The backup stashes what arrived
early, asks the primary's retransmitter for exactly that range, and re-asks after 20 ms without progress; duplicates are
ignored. Idle heartbeats carry the next sequence number, because without them a lost *final* datagram is indistinguishable
from silence. With 2% of datagrams dropped on purpose, the backup repaired every gap and ended with an identical digest.

**When is a client's order acknowledged, and what survives a crash?**
With `--replicate-wait`, only after the backup has acknowledged the command, and the backup acknowledges only what it has
applied and flushed to its own journal. So a promoted backup holds every acknowledged order. In one kill -9 run the client
had been acknowledged 314,081 commands and the backup held 314,680, an exact prefix of the dead primary's journal; it was
serving new orders 361 ms after the kill. Commands the primary sequenced but never replicated would be lost with it, and
none of them can have been acknowledged.

**What went wrong while building it?**
The journal-prefix check failed once: the backup held 25 commands more than the dead primary's journal. The publisher had
sent a datagram whenever 25 commands accumulated, before the batch's journal flush, so a `kill -9` could destroy commands
the backup already had. No acknowledged order was at risk (acknowledgements wait for the backup), but it broke write-ahead
ordering, and the earlier passing runs had been timing luck. Now the publisher only stages until the journal is flushed.

**What does this not handle?**
It runs on one host with loopback multicast. There is one backup, no snapshot for a replica that falls behind the ring, no
re-replication after a promotion, and no fencing: a primary that is partitioned rather than dead would not know it had been
replaced, so a real deployment needs leases or an arbiter. If the backup stops acknowledging, the primary waits 500 ms, says
so, and continues alone (availability over durability, explicitly).

## The benchmark environment

**These numbers are from a laptop under WSL2. Why trust them?**
Trust ratios within a run, not absolutes. A jitter probe shows the hypervisor taking ~6-12% of a pinned core, up to 10 ms at a
time, so wall-clock throughput drifts up to 2x between runs and tail latency above ~p99 reflects the hypervisor. Mitigations:
interleaved repetitions, best-of-N *and* median with bootstrap confidence intervals, a deterministic cache simulator for layout
questions. What is missing: bare-metal runs with isolated cores and hardware counters.

**Is 17x over Liquibook a fair claim?**
It is a real measurement on identical input with identical results, but not a like-for-like feature comparison. Liquibook
supports stop orders, depth tracking and per-operation callbacks, and stores levels in a `std::multimap`. Read it as "a
conventional design costs this much on this workload", not "Liquibook is slow".

## The market-making study

**Why does every strategy lose money?**
On these captures, at zero fees, with a queue-aware fill model and 10 ms latency, passive quotes are filled disproportionately
when the price is about to move against them (on BTC the mid moves $4.7 to $10 against a fresh fill within 1 s, against an unconditional 1 s standard deviation of $3.49 and mean absolute move of $1.12). Spread capture
is cents; inventory loss is dollars. Fees make it worse: at 10 bps every strategy loses $55 to $420 in half an hour.

**Does Avellaneda-Stoikov beat a fixed spread?**
Against an equal-width fixed spread, only at high risk aversion, and only by trading much less (0.58 BTC vs 1.99): it loses
less, it does not earn more. At lower risk aversion the paired 95% interval includes zero. Interpret it as inventory control,
not edge. See RESEARCH.md for the intervals and for what one 30-minute trending session cannot tell you.

**What would change your conclusion?**
A different regime (this BTC session trended up), maker rebates (I test -0.5 bps but a real rebate tier depends on volume),
or a lower-latency queue position than my model assumes. The cancellation proration was the other candidate, and I tested
it: order-by-order data shows cancellations come mostly from the back of long queues, so proportional proration is
optimistic, but rerunning the grid with the rule that fits the ground truth best (power 3) leaves the conclusion intact.

**How does the fill model work, and where is it weakest?**
Virtual quotes join the back of the displayed queue. Trades at the price consume the queue ahead first; trades through the
price fill in full; unexplained level shrinkage is cancellation, prorated by queue share. Weakest points: no own market
impact, no hidden/iceberg orders, book data at 100 ms granularity (so "mid at fill" can be up to 100 ms stale), and the
proration rule, which RESEARCH.md part 2 scores against Coinbase's queue: proportional predicts 15.1% of fills too early,
power 3 (now `--cancel-power 3`) 7.6%. The optimistic-fill comparison quantifies the model's importance: for touch quoting a naive backtest
counts ~7x the fills.

## Process

**What bug taught you the most?**
The failing real-data validation. It looked like a mirror bug (147 of 573 snapshots failed). The actual cause was a property of
diff-based reconstruction: unchanged levels deeper than the seed snapshot are invisible. Fixing it meant defining exactly
what the mirror can vouch for, and reseeding when the market outgrows that region. The lesson was to distrust a failing check
long enough to find out *why*, not to loosen it.
