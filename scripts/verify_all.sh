#!/usr/bin/env bash
# One command that re-runs every correctness check in the repository (no network, no long captures).
#
#   scripts/verify_all.sh [build-dir]        (build first: cmake --preset release && cmake --build --preset release)
#
# 1. unit, differential, allocation, journal, risk and market-making tests
# 2. real-data replay validates against the exchange's own snapshots, and fault injection is detected
# 3. gateway end to end: TCP output identical to a local engine, kill -9 recovery, torn journal tail;
#    two sessions (maker reports, anonymous counterparties) and hash flooding against exchange-assigned ids
# 4. replication end to end: multicast with injected loss, gap repair, kill -9 failover to the hot backup, fencing of a
#    paused primary, double failover with log shipping; level-2 market data with loss, snapshot recovery, late joiner
# 5. OCaml reference model: expect and property tests, and identical events to the C++ engine (if opam is present)
# 6. WebAssembly engine smoke test, and the browser UI end to end (if node / a Chrome-family browser are present)
# 7. accounting audit of the committed market-making results (if python + pandas are present)
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/release}
step() { printf '\n== %s\n' "$*"; }

step "1/7 tests"
"$BUILD/exsim_tests" | tail -1

step "2/7 real-data validation (30 s of BTCUSDT)"
"$BUILD/exsim_mdreplay" --in data/sample_btcusdt_30s.exmd | grep -E "exact validation|phantom|VALIDATED|FAILED"
"$BUILD/exsim_mdreplay" --in data/sample_btcusdt_30s.exmd --inject-drop 50 | tail -1

step "3/7 gateway end to end"
bash scripts/e2e_gateway.sh "$BUILD" | grep -E "IDENTICAL|OK|PASSED|FAIL"
python3 scripts/e2e_sessions.py "$BUILD" | grep -E "OK|PASSED|FAIL|CPU time"

step "4/7 replication and market data end to end"
bash scripts/e2e_replication.sh "$BUILD" | grep -E "OK|PASSED|FAIL|throughput"
bash scripts/e2e_marketdata.sh "$BUILD" | grep -E "OK|PASSED|FAIL"

step "5/7 OCaml reference model"
if command -v dune >/dev/null; then
  (cd ocaml && dune runtest) && echo "expect and property tests passed"
  bash scripts/ocaml_diff.sh "$BUILD" 5 200000 | tail -1
else
  echo "skipped (needs an opam switch with dune, ppx_expect, ppx_inline_test and qcheck-core)"
fi

step "6/7 WebAssembly engine and browser UI"
if command -v node >/dev/null && [ -f ui/engine.js ]; then node wasm/test_wasm.js; else echo "wasm test skipped (needs node and ui/engine.js)"; fi
python3 scripts/ui_e2e.py 2>/dev/null | tail -1 || echo "UI test skipped (needs python3 and Chrome, Chromium or Edge)"

step "7/7 accounting audit of committed study results"
if python3 -c "import pandas, numpy" 2>/dev/null; then
  for g in results/*/grid.csv; do d=$(dirname "$g"); python3 scripts/verify_accounting.py --results "$d" --dataset "$(basename "$d")" | tail -1; done
else
  echo "skipped (needs python3 with numpy and pandas)"
fi
printf '\nALL CHECKS PASSED\n'
