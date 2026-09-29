# FAQ

Short answers, each pointing at the code or measurement behind it.

## The engine

**What happens when a market buy arrives?**
`OrderBook::add` validates it (id, quantity, duplicate) and emits `Accepted`. Then `sweep` runs: while the order has
quantity left and `best_ask_ <= limit` (for a market order the limit is the top of the price band), it takes the head of
the best level's FIFO, emits a `Trade` at the maker's price, and shrinks or releases the maker. When a level empties it
clears that level's bit and asks the bitmap for the next one. A market order's leftover is cancelled (`IocExpired`),
never rested. See `sweep` in [order_book.hpp](../include/exsim/order_book.hpp).

**Why is cancel O(1)?**
The id index finds the order's slot (one probe in the common case), and the slot is unlinked from an intrusive doubly
linked list: two pointer writes. If that empties the level, one bit is cleared. Nothing is searched or shifted.

**Why a bitmap for the next best price?**
Real books are sparse: on BTC the next level can be hundreds of ticks away. A two-level bitmap finds it with at most two
word scans plus a scan of a 16-word summary, however big the gap. A test checks it against `std::set` over 50,000
random operations per size.

**What does modify do?**
Shrinking the size at the same price amends in place and keeps queue priority. A price change or a size increase is a
cancel/replace: the order goes to the back of the queue and may trade immediately. Coinbase's feed confirms both rules.

## Performance

**Your first optimized version was slower than `std::map`. Why?**
It ran 60% fewer instructions with 84% fewer branch misses, so the time was going to memory. The id index was sized for
capacity (8 MiB) but held about 4,000 live orders, and a well-mixed hash scattered them across the whole table, so nearly
every lookup touched a cold cache line and a cold page. A hash that keeps sequential ids near each other gave 2.3x.
[DESIGN.md §4](DESIGN.md#4-what-the-measurements-changed).

**Isn't that hash attackable?**
Yes. Keys built against the `k ^ (k >> bits)` fold all land in one home slot, and lookups go from about 2-3 ns to
770-950 ns. Switching to `fmix64` would not help, because it is invertible and an attacker can target it too. The fix is
what real venues do: clients never choose the engine's keys. The gateway assigns sequential exchange ids, and the one
table keyed by client input uses SipHash-1-3 with a random key. Over TCP, 40,000 colliding client ids cost the same server
CPU as sequential ones, and 39-59x more when passed straight to the engine (`scripts/e2e_sessions.py`).

**Tell me about a performance bug you found.**
The hash-flooding test's *baseline* was slow: 40,000 resting orders with ordinary sequential ids, cancelled oldest first,
spent 81% of all instructions in the id index. Consecutive ids fill one contiguous run of the table, and linear-probing
deletion has to scan to the next empty slot, so every cancel scanned the rest of the run: O(n²). Robin Hood insertion
keeps each run sorted by home slot, so deletion can stop at the first entry already in its home slot. Oldest-first erase
went from 56,517 ns to 3.4 ns at 40,000 live orders (`exsim_bench_index`).

**Why did structure-of-arrays lose?**
Every operation touches most fields of *one* order, so splitting fields across nine arrays turns one cache line into up
to nine. In the cache simulator pure SoA had 2.1x the L1 misses and 3.5x the last-level misses. SoA wins when a loop
streams one field across many elements; an order book does random access to whole records.

**So which layout is fastest?**
Wall-clock can't tell: the bootstrap intervals for `aos`, `soa` and `hybrid` overlap (3.11x [2.18, 3.85], 2.68x
[2.18, 3.29] and 3.41x [1.60, 3.69] over the baseline in the second campaign). `aos` is the default because it has the
fewest simulated cache misses, which is repeatable. That is a weaker claim than "fastest", and I state it that way.

**Is 18-22x over Liquibook a fair comparison?**
It is a real measurement on identical input, and both engines produce the same 933,619 trades. It is not like-for-like
on features: Liquibook has stop orders, depth tracking and per-operation callbacks, and stores levels in a
`std::multimap`. Read it as what a conventional design costs on this workload, not as "Liquibook is slow".

**These numbers come from a laptop under WSL2. Why trust them?**
Trust ratios within a run, not absolutes. A jitter probe shows the hypervisor taking 6-37% of a pinned core, sometimes
for 10 ms at a time, so throughput drifts between runs and latency above about p99 measures the hypervisor. To cope,
repetitions are interleaved across implementations, I report best-of-N and the median with bootstrap intervals, and
layout questions go to a deterministic cache simulator. Bare-metal runs are what is missing.

## Correctness

**How do you know the engine is right?**
Several independent checks, each shown to fail when the engine is broken:
1. Differential testing against a naive `std::map` book: events byte-identical after every command, full book state
   compared every 50 commands.
2. A second implementation in OCaml, written functionally, that emits identical events on 10M random commands.
3. Liquibook: 933,619 trades and 96,761,274 lots, identical, on 3M commands.
4. Coinbase: twelve days of its order-by-order feed, every trade reproduced ([VALIDATION.md](VALIDATION.md)).
5. Binance: the rebuilt book equals the exchange's own snapshots, 573 of 573.
6. Planted bugs: every one in the table in [TESTING.md](TESTING.md) is caught.

**What don't the tests cover?**
The reference book and the OCaml model are both mine, so they share my reading of the rules: agreement between them
rules out implementation bugs, not specification mistakes. Coinbase is the independent specification, but only for what
Coinbase does and publishes. Time in force, post-only and accounts are not in its feed and are inferred. Liquibook only
covers Day and IOC limit orders and cancels.

**How do you know you didn't just tune the replay until it agreed?**
Three reasons. Every rule is a statement about Coinbase's observable behaviour, checked against the raw JSON, never a
per-day or per-order exception, and one binary produced all twelve days. The inferred inputs come only from the order's
own messages, never from what the engine predicted. And the check fails when the engine is wrong: dropping every 1,000th
cancel produces divergences within five minutes of data, and CI runs that on every push. Also, most arrivals simply rest,
so the number that matters is the 4.8 million that traded; every one of them matched.

**What did Coinbase's data teach you that its docs didn't?**
A modify that crosses publishes its matches *before* the `change`, whose size is the post-trade remainder, and a modify
that fills completely sends no `change` at all. The snapshot is taken while the stream runs, so newer messages appear
before it in the file. The archive has gaps, and Tardis records a fresh snapshot when it reconnects. Self-trade prevention
leaves traces in all four of Coinbase's modes. And the exchange occasionally reports an order filled when its published
matches don't add up (41 orders in twelve days).

## Durability and replication

**What does the journal guarantee?**
Write-ahead plus flush-before-acknowledge: any command a client saw acknowledged is in the journal. Torn tails are found
and discarded; a record that fails its CRC stops the read and everything after it is distrusted. Write errors are not
ignored: if a write or flush fails, the gateway stops acknowledging and closes its sessions. `--sync os` survives a
process crash but not power loss; `batch` and `every` add `fsync`. I have tested `kill -9`, not power loss.

**Why replicate the input rather than the state?**
The engine is a deterministic function of its ordered input, so the command stream *is* the state, just compressed. Any
replica that applies the same commands reaches the same book, and event digests confirm it. Shipping state would need
snapshots and a consistency protocol; shipping input needs ordering and gap repair.

**UDP loses packets. How does the backup know it has everything?**
Every datagram carries the sequence number of its first command, so a jump is a known range. The backup stashes what
arrived early, asks the primary's retransmitter for exactly that range, and asks again after 20 ms without progress.
Idle heartbeats carry the next sequence number, because otherwise a lost *final* datagram looks like silence. With 2% of
datagrams dropped on purpose, the backup repaired every gap (336 in the latest run) and ended with the same digest.

**When is an order acknowledged, and what survives a crash?**
With `--replicate-wait`, only after the backup has applied the command and flushed it to its own journal. So a promoted
backup holds every acknowledged order. In the latest run the client had 319,744 commands acknowledged at the `kill -9`,
the backup held 320,280, its journal was an exact prefix of the dead primary's, and it was serving orders 320 ms later.

**What if the primary is only paused, not dead?**
The backup can't tell, so it promotes itself under epoch 2. Two independent defences stop the old primary from
acknowledging anything the new one lacks. It listens to its own multicast group, and on hearing epoch 2 it fences
itself. And a primary whose backup stops acknowledging halts instead of carrying on alone, which works even if it never
hears the new epoch. The test freezes the primary with `SIGSTOP`, waits for the promotion, resumes it, and checks both
cases: the old primary stops, and everything it acknowledged is held by the new one.

**What went wrong while building replication?**
The journal-prefix check failed once: the backup held 25 more commands than the dead primary's journal. The publisher
sent a datagram as soon as 25 commands had accumulated, before the batch's journal flush, so `kill -9` could destroy
commands the backup already had. No acknowledged order was at risk, since acknowledgements wait for the backup, but it
broke write-ahead ordering, and the earlier passing runs had been luck. Now nothing is published until the journal is
flushed.

**How do clients see the book?**
A level-2 feed: after each committed batch, the new totals of the levels it touched plus its trades, in sequenced
multicast packets, with a full snapshot every N packets. A subscriber that detects a gap rebuilds from the next snapshot.
Replication asks for retransmission because a replica must never miss a command; market data uses snapshots because it
has many consumers and none is served individually. At 2% loss the subscriber spends much of its time waiting for a
snapshot, which is why real feeds also run a retransmission service.

**What doesn't it handle?**
One host, loopback multicast, one backup at a time. Failure is detected by timeout, not consensus, so safety under a
partition comes from fencing and the halt rule, which give up availability instead. A production system would put the
sequenced log behind consensus or an arbiter.

## Research

**Why does every market-making strategy lose money?**
Passive quotes get filled disproportionately when the price is about to move against them. On BTC the mid moves $4.7-$10
against a fresh fill within one second, when the typical one-second move is $1.12. Spread capture is cents and inventory
loss is dollars. At 10 bps of fees every strategy loses $55-$420 in half an hour.

**Does Avellaneda-Stoikov beat a fixed spread?**
Against a fixed spread of the same width, only at high risk aversion, and only by trading much less (0.58 BTC against
1.99). It loses less; it doesn't earn more. At lower risk aversion the paired 95% interval includes zero. It is inventory
control, not edge.

**How does the fill model work, and where is it weakest?**
A quote joins the back of the displayed queue. Trades at its price consume the queue ahead first, trades through its
price fill it in full, and unexplained shrinkage of the level is treated as cancellation. The weak spots are no market
impact of our own, no hidden orders, 100 ms book data, and where cancellations come from. I measured that last one on
Coinbase's order-by-order data: they come mostly from the back of long queues, so the common proportional rule is
optimistic. Rerunning the study with the rule that fits best (power 3) left the conclusions unchanged.

**Queue imbalance predicts the next move with AUC 0.69. Why isn't that money?**
Because the size of the predictable move is tiny. The best return predictor explains 1.1% of the variance of 10-second
returns out of sample, about 0.25 bp. Crossing the spread on BTC-USD is almost free, but the exchange fee is quoted in
whole basis points. The information is useful passively, for skewing or pulling quotes, which is where the adverse
selection in the market-making study comes from.
