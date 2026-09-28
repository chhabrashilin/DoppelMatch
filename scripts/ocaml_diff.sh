#!/usr/bin/env bash
# Cross-language differential test: the C++ engine against the OCaml reference model (ocaml/).
#
# For each seed, exsim_difffeed writes a random command stream, both implementations replay it, and their
# event streams must be byte-identical. On a mismatch the first differing line is printed with the command
# stream kept for reproduction.
#
#   scripts/ocaml_diff.sh <build-dir> [seeds=20] [commands-per-seed=200000]
set -euo pipefail

build=${1:?usage: ocaml_diff.sh <build-dir> [seeds] [commands]}
seeds=${2:-20}
n=${3:-200000}
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [ -n "${ML_EXE:-}" ]; then
  ml=$ML_EXE  # a prebuilt reference binary
else
  (cd "$root/ocaml" && dune build --root . ./bin/exsim_ref_replay.exe 2>&1)
  ml="$root/ocaml/_build/default/bin/exsim_ref_replay.exe"
fi
cxx="$build/exsim_difffeed"

total=0
for seed in $(seq 1 "$seeds"); do
  "$cxx" --gen "$n" --seed "$seed" --out "$work/cmds.txt"
  "$cxx" --in "$work/cmds.txt" --book hybrid 2>/dev/null > "$work/cxx.txt"
  "$ml" "$work/cmds.txt" > "$work/ml.txt"
  if ! cmp -s "$work/cxx.txt" "$work/ml.txt"; then
    keep="$PWD/ocaml_diff_seed$seed.txt"
    cp "$work/cmds.txt" "$keep"
    echo "seed $seed: MISMATCH (command stream kept in $keep)"
    diff "$work/cxx.txt" "$work/ml.txt" | head -10
    exit 1
  fi
  events=$(wc -l < "$work/cxx.txt")
  total=$((total + events))
  echo "seed $seed: $(head -1 "$work/cmds.txt"), $n commands, $events events identical"
done
echo "all $seeds seeds identical: $total events"
