#!/usr/bin/env bash
# Compare pascalast (INTERP / AOT) vs Free Pascal (fpc -O- / fpc -O3) on
# every bench/*.pas.  Each program is timed once with /usr/bin/time -f "%e".
#
# pascalast aliases `integer` to int64 (so e.g. fib(36) does not overflow).
# fpc's default `integer` is 16-bit, and even `{$MODE OBJFPC}` only widens
# it to 32-bit.  Several benches multiply / accumulate past 2^31, so we
# rewrite `integer` → `int64` in the fpc copy of each source so both
# implementations agree on the semantics being measured.
set -u
cd "$(dirname "$0")/.." || exit 1
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

PROLOG='{$MODE OBJFPC}{$H+}'

run_pascalast_both() {
  local bench=$1
  : > node_specialized.c
  out=$(make -s aot-bench BENCH="$bench" 2>&1)
  aot=$(echo    "$out" | awk '/^AOT/{getline; print $1; exit}')
  interp=$(echo "$out" | awk '/^INTERP/{getline; print $1; exit}')
  echo "$interp $aot"
}

# Compile under fpc with mode objfpc + integer aliased to int64.  Returns
# wall-clock seconds, "FAIL" if compile fails, or "DIFF" if the program
# runs but the output differs from pascalast's reference.
run_fpc() {
  local bench=$1
  local optflag=$2
  local src="$WORK/${bench}.pas"
  printf '%s\n' "$PROLOG" > "$src"
  # Whole-word `integer` (case-insensitive) → `int64` so both impls
  # share the same numeric range.  Anchor on word boundaries so words
  # like `integers` are left alone.
  sed -E 's/\b([Ii][Nn][Tt][Ee][Gg][Ee][Rr])\b/int64/g' "bench/${bench}.pas" >> "$src"
  if ! ( cd "$WORK" && fpc "$optflag" "${bench}.pas" >/dev/null 2>&1 ); then
    echo "FAIL"
    return
  fi
  local exe="$WORK/${bench}"
  local refout="$WORK/${bench}.pascalast.out"
  ./pascalast -q "bench/${bench}.pas" > "$refout" 2>/dev/null
  local actout="$WORK/${bench}.fpc.out"
  local timeout="$WORK/${bench}.fpc.time"
  /usr/bin/time -f "%e" -o "$timeout" "$exe" > "$actout" 2>/dev/null
  if ! diff -q "$refout" "$actout" >/dev/null 2>&1; then
    echo "DIFF"
    return
  fi
  cat "$timeout"
}

printf "%-22s %8s %8s %8s %8s\n" "bench" "interp" "fpc -O-" "fpc -O3" "AOT"
printf "%-22s %8s %8s %8s %8s\n" "-----" "------" "-------" "-------" "---"

for path in bench/*.pas; do
  bench=$(basename "$path" .pas)
  read interp aot <<< "$(run_pascalast_both "$bench")"
  fp_o0=$(run_fpc "$bench" "-O-")
  fp_o3=$(run_fpc "$bench" "-O3")
  printf "%-22s %8s %8s %8s %8s\n" "$bench" "$interp" "$fp_o0" "$fp_o3" "$aot"
done
