#!/bin/sh
# Measure the instruction-count cost of a candidate change, using optcarrot's
# AOT run.  Wall-clock on this box swings ~20% run to run, so fps cannot resolve
# a 1-3% difference; retired-instruction count is deterministic and can.
#
#   tools/tracecost.sh <label> <koruby-binary> [FRAMES]
#
# Bakes the code store fresh for the given binary (an SD store from a different
# build would not be used anyway), then counts the --compiled-only run.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
OPT="$HERE/../abruby/benchmark/optcarrot"
LABEL=$1
BIN=$2
case "$BIN" in /*) ;; *) BIN="$PWD/$BIN";; esac   # absolutize: we cd into $OPT below
FRAMES=${3:-30}
BUNDLE=/tmp/optc_bundle.rb

OPTC_MODE=build sh "$HERE/tools/optcarrot.sh" "$FRAMES" >/dev/null || exit 1
cd "$OPT" || exit 1
rm -rf code_store
CCACHE_DISABLE=1 "$BIN" --aot-compile "$BUNDLE" >/dev/null 2>&1 || { echo "$LABEL: bake failed"; exit 1; }
perf stat -e instructions,branches,branch-misses -x, -o /tmp/claude-1000/tc.$$ \
    "$BIN" --compiled-only "$BUNDLE" > /tmp/claude-1000/tc.out.$$ 2>&1
printf '%-12s fps=%-8s ' "$LABEL" "$(grep -o 'fps: [0-9.]*' /tmp/claude-1000/tc.out.$$ | cut -d' ' -f2 | cut -c1-7)"
printf 'checksum=%-6s ' "$(grep -o 'checksum: [0-9]*' /tmp/claude-1000/tc.out.$$ | cut -d' ' -f2)"
awk -F, '/^[0-9]/ { if ($3 == "instructions") printf "insn=%-14d ", $1;
                    if ($3 == "branches")     printf "br=%-13d ", $1;
                    if ($3 == "branch-misses")printf "brmiss=%d", $1 }' /tmp/claude-1000/tc.$$
echo
rm -f /tmp/claude-1000/tc.$$ /tmp/claude-1000/tc.out.$$
