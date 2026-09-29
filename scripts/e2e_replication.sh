#!/usr/bin/env bash
# End-to-end test of replication and failover: the primary (exsim_server) multicasts its sequenced command
# stream, and a hot backup (exsim_replica) applies it.
#
#   scripts/e2e_replication.sh [build-dir]
#
# R1. Lossless replication: after 500k client orders, the backup's event digest equals the primary's.
# R2. Lossy replication: the primary skips 2% of its multicast datagrams (fault injection); the backup
#     detects every gap, has it retransmitted, and still ends with an identical digest.
# R3. The cost of waiting for the backup: closed-loop throughput without replication, with asynchronous
#     replication, and with --replicate-wait (acks held until the backup has the command).
# R4. Failover: kill -9 the primary mid-stream with 1% datagram loss. The backup must
#       1. promote itself and accept orders on its own port,
#       2. hold every command the client saw acknowledged (none lost),
#       3. hold a journal that is an exact prefix of the dead primary's journal (no divergence), and
#       4. after taking more orders, have a state whose digest equals an offline replay of its journal.
# R5. Partition, not crash: the primary is frozen (SIGSTOP) under load, the backup promotes itself (epoch 2),
#     then the old primary resumes. It must fence itself (it hears epoch 2, or its backup never acknowledges
#     again) and never acknowledge an order the new primary lacks: no split brain.
# R6. Two failovers in a row. The first backup promotes and replicates under epoch 2. A fresh backup is too far
#     behind for the new primary's small retransmission ring and is refused explicitly; one started from a copy
#     of the new primary's journal (log shipping) catches up. The new primary is killed too; the second backup
#     promotes (epoch 3), holds every acknowledged order, and its state equals an offline replay of its journal.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/release}
W=$(mktemp -d)
BASE=$((20000 + RANDOM % 20000))
PORT=$BASE PORT2=$((BASE + 1)) CTRL=$((BASE + 2)) GROUP="239.255.0.1:$((BASE + 3))"
PORT3=$((BASE + 4)) CTRL2=$((BASE + 5)) CTRL3=$((BASE + 6))
PIDS=()
trap 'for p in "${PIDS[@]:-}"; do kill -9 "$p" 2>/dev/null || true; done; rm -rf "$W"' EXIT

wait_for() {  # wait_for <pattern> <log>
  for _ in $(seq 200); do grep -q "$1" "$2" 2>/dev/null && return 0; sleep 0.05; done
  echo "timed out waiting for '$1' in $2"; cat "$2"; exit 1
}
field() { sed -n "s/.*$1=\([0-9a-f.]*\).*/\1/p" "$2" | tail -1; }
start_primary() {  # start_primary <log> [extra args...]
  local log=$1; shift
  "$BUILD/exsim_server" --port "$PORT" --journal "$W/p.bin" "$@" > "$log" 2>&1 &
  PRIMARY=$!; PIDS+=("$PRIMARY")
  wait_for READY "$log"
}
start_replica() {  # start_replica <log> [extra args...]
  local log=$1; shift
  "$BUILD/exsim_replica" --group "$GROUP" --primary "127.0.0.1:$CTRL" --journal "$W/r.bin" "$@" > "$log" 2>&1 &
  REPLICA=$!; PIDS+=("$REPLICA")
  wait_for REPLICA_READY "$log"
}
replicated_run() {  # replicated_run <name> <drop>
  local name=$1 drop=$2
  start_replica "$W/$name.r.log"
  start_primary "$W/$name.p.log" --publish "$GROUP" --control-port "$CTRL" --replicate-wait --drop "$drop"
  "$BUILD/exsim_client" --port "$PORT" --messages 500000 --window 256 > "$W/$name.c.log"
  grep -q IDENTICAL "$W/$name.c.log" || { echo "FAIL: client verification"; cat "$W/$name.c.log"; exit 1; }
  kill -TERM "$PRIMARY"; wait "$PRIMARY" || true
  wait "$REPLICA" || { echo "FAIL: replica exited with an error"; cat "$W/$name.r.log"; exit 1; }
  grep -E "PUBLISHED|STOPPED" "$W/$name.p.log"
  grep -E "^END" "$W/$name.r.log"
  local pd rd
  pd=$(field digest "$W/$name.p.log"); rd=$(field digest "$W/$name.r.log")
  [ -n "$pd" ] && [ "$pd" = "$rd" ] && echo "primary digest == backup digest ($pd): OK" \
    || { echo "FAIL: digests differ (primary $pd, backup $rd)"; exit 1; }
}

echo "=== R1. lossless replication (500k orders) ==="
replicated_run r1 0

echo; echo "=== R2. 2% of multicast datagrams dropped by the primary ==="
replicated_run r2 0.02
GAPS=$(field gaps "$W/r2.r.log")
[ "$GAPS" -gt 0 ] && echo "backup detected and repaired $GAPS gaps: OK" || { echo "FAIL: no gaps seen with 2% loss"; exit 1; }

echo; echo "=== R3. throughput: no replication vs async vs replicate-wait ==="
for mode in none async wait; do
  case $mode in
    none) start_primary "$W/r3.p.log" ;;
    async) start_replica "$W/r3.r.log"; start_primary "$W/r3.p.log" --publish "$GROUP" --control-port "$CTRL" ;;
    wait) start_replica "$W/r3.r.log"; start_primary "$W/r3.p.log" --publish "$GROUP" --control-port "$CTRL" --replicate-wait ;;
  esac
  printf "%-6s " "$mode"; "$BUILD/exsim_client" --port "$PORT" --messages 1000000 --window 256 | grep throughput
  kill -TERM "$PRIMARY"; wait "$PRIMARY" || true
  [ "$mode" != none ] && { wait "$REPLICA" || true; }
done

echo; echo "=== R4. failover: kill -9 the primary mid-stream ==="
start_replica "$W/r4.r.log" --promote-port "$PORT2" --silence-ms 300
start_primary "$W/r4.p.log" --publish "$GROUP" --control-port "$CTRL" --replicate-wait --drop 0.01
( "$BUILD/exsim_client" --port "$PORT" --messages 3000000 --window 1024 --no-verify > "$W/r4.c.log" 2>&1 || true ) &
CLI=$!
for _ in $(seq 600); do
  [ "$(stat -c %s "$W/p.bin" 2>/dev/null || echo 0)" -gt 20000000 ] && break
  sleep 0.05
done
KILL_NS=$(date +%s%N)
kill -9 "$PRIMARY"; wait "$PRIMARY" 2>/dev/null || true               # the crash
wait "$CLI" || true
ACKED=$(awk '/^ACKED/ {print $2}' "$W/r4.c.log")
wait_for "READY port=$PORT2" "$W/r4.r.log"
UP_NS=$(date +%s%N)
grep PROMOTING "$W/r4.r.log"
echo "client saw $ACKED commands acknowledged before the crash"
echo "backup serving orders $(( (UP_NS - KILL_NS) / 1000000 )) ms after the kill (silence threshold 300 ms)"
HELD=$(field seq "$W/r4.r.log")
[ "$HELD" -ge "$ACKED" ] && echo "no acknowledged command lost: backup holds $HELD >= acked $ACKED: OK" \
  || { echo "FAIL: backup holds $HELD < acked $ACKED"; exit 1; }
"$BUILD/exsim_journal" prefix "$W/r.bin" "$W/p.bin" || { echo "FAIL: backup journal diverges from the primary's"; exit 1; }
echo "backup journal is an exact prefix of the primary's: OK"

"$BUILD/exsim_client" --port "$PORT2" --messages 200000 --window 256 --seed 7 --no-verify | grep -E "throughput|ACKED"
kill -TERM "$REPLICA"; wait "$REPLICA" || true
grep STOPPED "$W/r4.r.log"
LIVE=$(field digest "$W/r4.r.log")
OFFLINE=$("$BUILD/exsim_journal" replay "$W/r.bin")
echo "offline replay of the backup's journal: $OFFLINE"
[ "$LIVE" = "$(echo "$OFFLINE" | sed -n 's/.*digest=\([0-9a-f]*\).*/\1/p')" ] \
  && echo "promoted state digest == offline replay digest: OK" || { echo "FAIL: promoted state diverges from its journal"; exit 1; }

wait_journal() {  # wait_journal <file> <bytes>
  for _ in $(seq 600); do [ "$(stat -c %s "$1" 2>/dev/null || echo 0)" -gt "$2" ] && return 0; sleep 0.05; done
  echo "FAIL: $1 never reached $2 bytes"; exit 1
}
same_digest_as_replay() {  # same_digest_as_replay <log> <journal>
  local live offline
  live=$(field digest "$1")
  offline=$("$BUILD/exsim_journal" replay "$2" | sed -n 's/.*digest=\([0-9a-f]*\).*/\1/p')
  [ -n "$live" ] && [ "$live" = "$offline" ] && echo "promoted state digest == offline replay digest ($live): OK" \
    || { echo "FAIL: promoted state ($live) differs from a replay of its journal ($offline)"; exit 1; }
}

partition_run() {  # partition_run <announce|silent>
  # announce: the new primary publishes epoch 2 on the group, so the old one learns it was replaced (FENCED).
  # silent:   the new primary does not publish; the old one only sees that its backup stopped acknowledging
  #           (REPLICA_LOST), and must halt on that alone.
  local mode=$1 expect=FENCED ready="READY port=$PORT2 .*epoch=2" extra=(--promote-control-port "$CTRL2")
  [ "$mode" = silent ] && expect=REPLICA_LOST ready="READY port=$PORT2" extra=()
  rm -f "$W"/*.bin
  start_replica "$W/r5$mode.r.log" --promote-port "$PORT2" --silence-ms 300 "${extra[@]}"
  start_primary "$W/r5$mode.p.log" --publish "$GROUP" --control-port "$CTRL" --replicate-wait
  ( "$BUILD/exsim_client" --port "$PORT" --messages 3000000 --window 1024 --no-verify > "$W/r5$mode.c.log" 2>&1 || true ) &
  CLI=$!
  wait_journal "$W/p.bin" 20000000
  kill -STOP "$PRIMARY"                                          # frozen, not dead
  wait_for "$ready" "$W/r5$mode.r.log"
  sleep 0.3
  kill -CONT "$PRIMARY"                                          # the old primary wakes up
  for _ in $(seq 200); do kill -0 "$PRIMARY" 2>/dev/null || break; sleep 0.05; done
  if kill -0 "$PRIMARY" 2>/dev/null; then                        # still serving 10 s later: split brain
    kill -9 "$PRIMARY"; wait "$CLI" || true
    echo "FAIL: the old primary kept serving after being replaced"; tail -3 "$W/r5$mode.p.log"; exit 1
  fi
  wait "$PRIMARY" 2>/dev/null || true
  wait "$CLI" || true
  grep -m1 -E "FENCED|REPLICA_LOST.*halting" "$W/r5$mode.p.log"
  grep -q "reason=$expect" "$W/r5$mode.p.log" && echo "the resumed primary halted ($expect) instead of serving: OK" \
    || { echo "FAIL: the resumed primary did not stop for the expected reason ($expect)"; cat "$W/r5$mode.p.log"; exit 1; }
  ACKED=$(awk '/^ACKED/ {print $2}' "$W/r5$mode.c.log")
  HELD=$(field seq "$W/r5$mode.r.log")
  [ "$HELD" -ge "$ACKED" ] && echo "no split brain: all $ACKED acknowledged commands are in the new primary ($HELD held): OK" \
    || { echo "FAIL: the old primary acknowledged $ACKED but the new one holds $HELD"; exit 1; }
  "$BUILD/exsim_journal" prefix "$W/r.bin" "$W/p.bin" > /dev/null \
    && echo "new primary's journal is an exact prefix of the old one's (its extra commands were never acknowledged): OK" \
    || { echo "FAIL: journals diverge"; exit 1; }
  kill -TERM "$REPLICA"; wait "$REPLICA" || true
}

echo; echo "=== R5. partition: pause the primary (SIGSTOP), let the backup take over, resume the old primary ==="
echo "--- the new primary announces epoch 2"
partition_run announce
echo "--- the new primary is silent"
partition_run silent

echo; echo "=== R6. two failovers in a row, with log shipping for the second backup ==="
rm -f "$W"/*.bin
start_replica "$W/r6a.log" --promote-port "$PORT2" --silence-ms 300 --promote-control-port "$CTRL2" \
  --promote-replicate-wait --promote-ring-log2 12
B1=$REPLICA
start_primary "$W/r6.p.log" --publish "$GROUP" --control-port "$CTRL" --replicate-wait --epoch 1
"$BUILD/exsim_client" --port "$PORT" --messages 200000 --window 256 > "$W/r6.c1.log"
grep -q IDENTICAL "$W/r6.c1.log" || { echo "FAIL: client 1 verification"; exit 1; }
kill -9 "$PRIMARY"; wait "$PRIMARY" 2>/dev/null || true         # first failure
wait_for "READY port=$PORT2 .*epoch=2" "$W/r6a.log"
echo "first backup promoted: epoch 2, serving and replicating"
# A backup starting from nothing is 200,000 commands behind a 4,096-command ring: it must be refused, not stall.
timeout 20 "$BUILD/exsim_replica" --group "$GROUP" --primary "127.0.0.1:$CTRL2" --journal "$W/r_fresh.bin" > "$W/r6fresh.log" 2>&1 || true
grep -q FATAL_UNAVAILABLE "$W/r6fresh.log" && echo "a backup too far behind the ring is refused explicitly: OK" \
  || { echo "FAIL: expected FATAL_UNAVAILABLE"; cat "$W/r6fresh.log"; exit 1; }
cp "$W/r.bin" "$W/shipped.bin"                                    # log shipping: a copy of the new primary's journal
"$BUILD/exsim_replica" --group "$GROUP" --primary "127.0.0.1:$CTRL2" --journal "$W/r2.bin" --from-journal "$W/shipped.bin" \
  --promote-port "$PORT3" --silence-ms 300 --promote-control-port "$CTRL3" > "$W/r6b.log" 2>&1 &
B2=$!; PIDS+=("$B2")
wait_for REPLICA_READY "$W/r6b.log"
grep BOOTSTRAPPED "$W/r6b.log"
"$BUILD/exsim_client" --port "$PORT2" --messages 200000 --window 256 --seed 7 --no-verify > "$W/r6.c2.log" 2>&1 || true
ACKED2=$(awk '/^ACKED/ {print $2}' "$W/r6.c2.log")
[ "$ACKED2" -eq 200000 ] || { echo "FAIL: client 2 acknowledged $ACKED2 of 200000"; exit 1; }
kill -9 "$B1"; wait "$B1" 2>/dev/null || true                     # second failure: the promoted primary dies too
wait_for "READY port=$PORT3 .*epoch=3" "$W/r6b.log"
echo "second backup promoted: epoch 3"
cp "$W/r2.bin" "$W/r2_at_promotion.bin"
HELD=$(field seq "$W/r6b.log")
[ "$HELD" -ge 400000 ] && echo "every acknowledged command of both clients survived two failovers ($HELD held >= 400000): OK" \
  || { echo "FAIL: only $HELD commands held"; exit 1; }
"$BUILD/exsim_journal" prefix "$W/r2_at_promotion.bin" "$W/r.bin" > /dev/null \
  && echo "second backup's journal is an exact prefix of the first backup's: OK" || { echo "FAIL: journals diverge"; exit 1; }
"$BUILD/exsim_client" --port "$PORT3" --messages 100000 --window 256 --seed 11 --no-verify | grep -E "ACKED"
kill -TERM "$B2"; wait "$B2" || true
same_digest_as_replay "$W/r6b.log" "$W/r2.bin"

echo; echo "ALL REPLICATION CHECKS PASSED"
