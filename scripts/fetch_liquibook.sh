#!/usr/bin/env bash
# Fetches Liquibook (https://github.com/enewhuis/liquibook) for the head-to-head benchmark.
# It is deliberately not vendored: it keeps its own license and this repository stays dependency-free.
# The checkout is pinned to one commit, so the benchmark's baseline cannot change under it.
#
#   scripts/fetch_liquibook.sh [dest]        then:  cmake -DEXSIM_LIQUIBOOK_DIR=<dest> ...
set -euo pipefail
DEST=${1:-third_party/liquibook}
COMMIT=2427613b32f1667abae68a01df6af9ba8270f8e7   # master as of 2022-12-15, the version benchmarked in BENCHMARKS.md
if [ ! -d "$DEST/.git" ]; then
  git init -q "$DEST"
  git -C "$DEST" fetch -q --depth 1 https://github.com/enewhuis/liquibook.git "$COMMIT"
  git -C "$DEST" checkout -q FETCH_HEAD
fi
[ "$(git -C "$DEST" rev-parse HEAD)" = "$COMMIT" ] || { echo "liquibook checkout is not at $COMMIT" >&2; exit 1; }
git -C "$DEST" log -1 --format='liquibook at %H (%cd)'
