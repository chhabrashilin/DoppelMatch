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
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/release}
W=$(mktemp -d)
BASE=$((20000 + RANDOM % 20000))
PORT=$BASE PORT2=$((BASE + 1)) CTRL=$((BASE + 2)) GROUP="239.255.0.1:$((BASE + 3))"
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
echo; echo "ALL REPLICATION CHECKS PASSED"
