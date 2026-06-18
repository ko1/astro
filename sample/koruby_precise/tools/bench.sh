#!/usr/bin/env bash
# koruby_precise benchmark runner.
#
# Runs the shared rubyharness suite across modes and saves a timestamped report
# (date + git short-hash) under bench-report/, also echoing to the terminal.
#
# The AOT modes (aot+compile / aot+cached) run with --compiled-only, so an AOT
# run that would silently fall back to the interpreter aborts (exit 7 → INTERP!
# in the table) instead of reporting a misleadingly fast number.  @noinline
# compile-exempt bodies (proc/class/module roots) are not flagged.
#
# Usage:
#   tools/bench.sh                       # default modes, 5 runs, whole suite
#   RUNS=3 tools/bench.sh                # fewer iterations
#   MODES=interp,aot+cached tools/bench.sh
#   tools/bench.sh --pattern '{fib,nbody}.rb'   # extra run_bench args pass through
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)          # sample/koruby_precise
HARNESS="$HERE/../rubyharness"
REPORT_DIR="$HERE/bench-report"
mkdir -p "$REPORT_DIR"

cd "$HERE"
echo "building koruby_precise ..." >&2
make >/dev/null 2>&1 || { echo "bench.sh: build failed"; exit 1; }

stamp=$(date +%Y%m%d-%H%M%S)
hash=$(git rev-parse --short HEAD 2>/dev/null || echo nogit)
dirty=$(git diff --quiet HEAD -- "$HERE" 2>/dev/null && echo "" || echo "-dirty")
out="$REPORT_DIR/${stamp}-${hash}${dirty}.txt"

modes="${MODES:-cruby,cruby+yjit,interp,aot+compile,aot+cached}"
runs="${RUNS:-5}"

{
  echo "# koruby_precise bench   $stamp   ${hash}${dirty}"
  echo "# modes: $modes   runs: $runs   (aot modes run --compiled-only)"
  echo "# host:  $(uname -sr)"
  echo "# ruby:  $(ruby -v 2>/dev/null | cut -c1-60)"
  echo
  ruby "$HARNESS/tools/run_bench.rb" \
       --interp "$HERE/koruby_precise" --dir "$HARNESS/bench" \
       --modes "$modes" --runs "$runs" --aot-flags '--compiled-only' "$@"
} 2>&1 | tee "$out"

echo
echo "saved: $out" >&2
