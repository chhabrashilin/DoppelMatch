#!/usr/bin/env bash
# The OCaml model on real data: the Coinbase replay records the exact command stream its engine received
# (--emit-commands), then the C++ engine and the OCaml model both replay that stream and their event streams must be
# byte-identical. Random streams test the rules broadly; a real day tests them on what a market actually sends.
#
#   scripts/ocaml_real.sh <build-dir> <day.exl3>     (build-dir needs exsim_l3replay and exsim_difffeed)
set -euo pipefail

build=${1:?usage: ocaml_real.sh <build-dir> <day.exl3>}
in=${2:?usage: ocaml_real.sh <build-dir> <day.exl3>}
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [ -n "${ML_EXE:-}" ]; then
  ml=$ML_EXE  # a prebuilt reference binary
else
  (cd "$root/ocaml" && dune build --root . ./bin/exsim_ref_replay.exe 2>&1)
  ml="$root/ocaml/_build/default/bin/exsim_ref_replay.exe"
fi

"$build/exsim_l3replay" --in "$in" --check-every 0 --emit-commands "$work/cmds.txt" | grep -E "reproduced exactly|predicted exactly"
"$build/exsim_difffeed" --in "$work/cmds.txt" --book aos 2> "$work/cxx.err" > "$work/cxx.txt"
"$ml" "$work/cmds.txt" 2> /dev/null > "$work/ml.txt"
events=$(wc -l < "$work/cxx.txt")
if cmp -s "$work/cxx.txt" "$work/ml.txt"; then
  echo "real data: $(cut -d' ' -f1 "$work/cxx.err") commands, $events events, identical in C++ and OCaml: OK"
else
  echo "FAIL: the C++ engine and the OCaml model differ on real data"
  diff "$work/cxx.txt" "$work/ml.txt" | head -10
  exit 1
fi
