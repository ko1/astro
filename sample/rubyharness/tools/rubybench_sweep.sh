#!/bin/sh
# rubybench_sweep.sh — run every ruby/ruby-bench single-file micro through the
# sample (INTERP) and the reference (RUBY) and report which agree.  Self-
# maintaining: no hardcoded pass list, so new/removed micros are picked up.
# Ractor benches are skipped (no Ractor in the ASTro Ruby subset).
#
#   INTERP=./koruby_precise sh tools/rubybench_sweep.sh   [BENCH_ITRS=1]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../apps/ruby-bench"
INTERP=${INTERP:-./koruby_precise}
RUBY=${RUBY:-ruby}
[ -d "$APP/benchmarks" ] || sh "$HERE/rubybench_setup.sh"
case "$INTERP" in /*|*" "*) ;; ./*) INTERP="$(cd "$(dirname "$INTERP")" && pwd)/$(basename "$INTERP")";; esac

ok=0; mm=0; fail=0; skip=0
printf '%-26s %-12s %-12s %s\n' BENCH ref sample status
for f in "$APP"/benchmarks/*.rb; do
  b=$(basename "$f" .rb)
  if grep -q 'ractor_args' "$f"; then printf '%-26s %s\n' "$b" 'skip (ractor)'; skip=$((skip+1)); continue; fi
  bundle=$(BENCH="$b" RB_MODE=build sh "$HERE/rubybench.sh")
  cr=$(BENCH_ITRS=1 timeout 60 "$RUBY" --yjit-disable "$bundle" 2>/dev/null | tail -1)
  ko=$(BENCH_ITRS=1 timeout 60 "$INTERP" --plain "$bundle" 2>/dev/null | tail -1)
  if   [ -z "$ko" ];        then st='FAIL';     fail=$((fail+1))
  elif [ "$cr" = "$ko" ];   then st='ok';       ok=$((ok+1))
  else                           st='MISMATCH'; mm=$((mm+1)); fi
  printf '%-26s %-12.12s %-12.12s %s\n' "$b" "$cr" "$ko" "$st"
done
echo "---"
echo "ok=$ok mismatch=$mm fail=$fail skip=$skip"
