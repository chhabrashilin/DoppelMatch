#!/usr/bin/env bash
# Line and branch coverage of the engine and infrastructure headers (include/exsim/), from the unit and
# differential tests, the gateway, session and replication end-to-end tests, and a cross-language differential run.
# Clang source-based coverage: build with -DEXSIM_COVERAGE=ON.
#
#   scripts/coverage.sh [build-dir]       (needs clang, llvm-profdata and llvm-cov on PATH, or LLVM_SUFFIX=-18)
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/coverage}
S=${LLVM_SUFFIX:-}
PROF=$(mktemp -d)
export LLVM_PROFILE_FILE="$PROF/%p-%m.profraw"   # one file per process, so concurrent servers do not clobber

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_CXX_COMPILER="clang++$S" -DCMAKE_BUILD_TYPE=Debug -DEXSIM_COVERAGE=ON \
  -DEXSIM_NATIVE=OFF > /dev/null
cmake --build "$BUILD" > /dev/null

"$BUILD/exsim_tests" | tail -1
bash scripts/e2e_gateway.sh "$BUILD" > /dev/null
python3 scripts/e2e_sessions.py "$BUILD" > /dev/null
bash scripts/e2e_replication.sh "$BUILD" > /dev/null
bash scripts/e2e_marketdata.sh "$BUILD" > /dev/null
"$BUILD/exsim_difffeed" --gen 200000 --seed 3 --out "$PROF/cmds.txt"
"$BUILD/exsim_difffeed" --in "$PROF/cmds.txt" --book aos > /dev/null 2>&1
"$BUILD/exsim_mdreplay" --in data/sample_btcusdt_30s.exmd > /dev/null
"$BUILD/exsim_mmsim" --in data/sample_btcusdt_30s.exmd --strategy as --warmup-s 5 > /dev/null

"llvm-profdata$S" merge -sparse "$PROF"/*.profraw -o "$PROF/all.profdata"
objs=()
for b in exsim_tests exsim_server exsim_replica exsim_mdlisten exsim_client exsim_journal exsim_difffeed exsim_mdreplay exsim_mmsim; do
  objs+=(-object "$BUILD/$b")
done
"llvm-cov$S" report "${objs[@]:1}" -instr-profile="$PROF/all.profdata" include/exsim/*.hpp tools/gateway.hpp \
  | sed 's|^.*/include/exsim/|include/exsim/|; s|^.*/tools/|tools/|'
rm -rf "$PROF"
