#!/usr/bin/env bash
# End-to-end test of the public market-data feed (include/exsim/mdfeed.hpp): the server publishes level-2
# incrementals and snapshots over multicast; exsim_mdlisten rebuilds every symbol's book from them.
#
#   scripts/e2e_marketdata.sh [build-dir]
#
# M1. Lossless: after 500k orders the subscriber's book equals the engine's (level-2 digests).
# M2. 2% of incremental packets dropped by the server (fault injection): the subscriber detects every gap, discards
#     its book, rebuilds from the next snapshot, and still ends equal.
# M3. Late joiner: a subscriber that starts halfway through synchronizes on the next snapshot and ends equal.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/release}
W=$(mktemp -d)
BASE=$((20000 + RANDOM % 20000))
PORT=$BASE MDG="239.255.0.2:$((BASE + 7))"
PIDS=()
trap 'for p in "${PIDS[@]:-}"; do kill -9 "$p" 2>/dev/null || true; done; rm -rf "$W"' EXIT

wait_for() {  # wait_for <pattern> <log>
  for _ in $(seq 200); do grep -q "$1" "$2" 2>/dev/null && return 0; sleep 0.05; done
  echo "timed out waiting for '$1' in $2"; cat "$2"; exit 1
}
field() { sed -n "s/.*$1=\([0-9a-f.]*\).*/\1/p" "$2" | tail -1; }

run() {  # run <name> <md-drop> <listener starts after this many journal bytes (0: before the traffic)>
  local name=$1 drop=$2 late=$3
  rm -f "$W/j.bin"
  "$BUILD/exsim_server" --port "$PORT" --journal "$W/j.bin" --md-publish "$MDG" --md-snapshot-every 200 --md-drop "$drop" \
    > "$W/$name.s.log" 2>&1 &
  local srv=$!; PIDS+=("$srv")
  wait_for READY "$W/$name.s.log"
  local lis=""
  if [ "$late" = 0 ]; then
    "$BUILD/exsim_mdlisten" --group "$MDG" > "$W/$name.l.log" 2>&1 & lis=$!; PIDS+=("$lis")
    wait_for MD_LISTENING "$W/$name.l.log"
  fi
  "$BUILD/exsim_client" --port "$PORT" --messages 500000 --window 256 > "$W/$name.c.log" 2>&1 &
  local cli=$!
  if [ "$late" != 0 ]; then
    for _ in $(seq 600); do [ "$(stat -c %s "$W/j.bin" 2>/dev/null || echo 0)" -gt "$late" ] && break; sleep 0.05; done
    "$BUILD/exsim_mdlisten" --group "$MDG" > "$W/$name.l.log" 2>&1 & lis=$!; PIDS+=("$lis")
  fi
  wait "$cli"
  grep -q IDENTICAL "$W/$name.c.log" || { echo "FAIL: client verification"; exit 1; }
  kill -TERM "$srv"; wait "$srv" || true
  wait "$lis" || { echo "FAIL: the subscriber did not reach the end of the feed"; cat "$W/$name.l.log"; exit 1; }
  grep MARKET_DATA "$W/$name.s.log"
  grep -E "^SYNCED" "$W/$name.l.log" | head -1
  grep MD_END "$W/$name.l.log"
  local a b
  a=$(field l2 "$W/$name.s.log"); b=$(field l2 "$W/$name.l.log")
  [ -n "$a" ] && [ "$a" = "$b" ] && echo "subscriber's book == engine's book (l2 $a): OK" \
    || { echo "FAIL: the subscriber's book ($b) differs from the engine's ($a)"; exit 1; }
}

echo "=== M1. lossless ==="
run m1 0 0
echo; echo "=== M2. 2% of incremental packets dropped ==="
run m2 0.02 0
GAPS=$(field gaps "$W/m2.l.log"); SNAPS=$(field snapshots_used "$W/m2.l.log")
[ "$GAPS" -gt 0 ] && [ "$SNAPS" -gt 1 ] && echo "subscriber detected $GAPS gaps and recovered through snapshots ($SNAPS used): OK" \
  || { echo "FAIL: expected gaps and snapshot recoveries (gaps $GAPS, snapshots $SNAPS)"; exit 1; }
echo; echo "=== M3. late joiner ==="
run m3 0 10000000
echo; echo "ALL MARKET-DATA CHECKS PASSED"
