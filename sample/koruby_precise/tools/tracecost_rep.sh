#!/bin/sh
# Bake once for the given binary, then run it N times under perf and print
# every run.  Minimum cycles is the estimator to read: it is the run least
# disturbed by other load, and this box swings ~20% run to run.
#   tools/tracecost_rep.sh <label> <koruby-binary> [RUNS] [FRAMES]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
OPT="$HERE/../abruby/benchmark/optcarrot"
LABEL=$1
BIN=$2
case "$BIN" in /*) ;; *) BIN="$PWD/$BIN";; esac
RUNS=${3:-5}
FRAMES=${4:-30}
BUNDLE=/tmp/optc_bundle.rb

OPTC_MODE=build sh "$HERE/tools/optcarrot.sh" "$FRAMES" >/dev/null || exit 1
cd "$OPT" || exit 1
rm -rf code_store "$HERE/preload_store"      # both stores are keyed to one binary; two binaries thrash them
CCACHE_DISABLE=1 "$BIN" --aot-compile "$BUNDLE" >/dev/null 2>&1 || { echo "$LABEL: bake failed"; exit 1; }
i=0
while [ "$i" -lt "$RUNS" ]; do
  i=$((i + 1))
  perf stat -e cycles,instructions -x, -o /tmp/claude-1000/tr.$$ \
      "$BIN" --compiled-only "$BUNDLE" > /tmp/claude-1000/tr.out.$$ 2>&1
  printf '%-11s run%-2d ' "$LABEL" "$i"
  awk -F, '/^[0-9]/ { if ($3 == "cycles")       printf "cyc=%-13d ", $1;
                      if ($3 == "instructions") printf "insn=%-13d ipc=%.3f", $1, $1/c }
           /^[0-9]/ { if ($3 == "cycles") c = $1 }' /tmp/claude-1000/tr.$$
  fps=$(grep -o 'fps: [0-9.]*' /tmp/claude-1000/tr.out.$$ | cut -d' ' -f2 | cut -c1-7)
  ck=$(grep -o 'checksum: [0-9]*' /tmp/claude-1000/tr.out.$$ | cut -d' ' -f2)
  if [ -z "$ck" ]; then printf ' RUN FAILED -- discard\n'; tail -2 /tmp/claude-1000/tr.out.$$; exit 1; fi
  printf ' fps=%-8s ck=%s\n' "$fps" "$ck"
done
rm -f /tmp/claude-1000/tr.$$ /tmp/claude-1000/tr.out.$$
