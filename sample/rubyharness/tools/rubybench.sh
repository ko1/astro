#!/bin/sh
# rubybench.sh — run a ruby/ruby-bench single-file micro benchmark through an
# ASTro sample.  koruby has no `require`, so we bundle: strip the
# `require_relative '../harness/loader'` and prepend a tiny koruby-compatible
# run_benchmark shim that runs the block BENCH_ITRS times and prints its result
# (deterministic → CRuby and the sample must agree).  Wall clock is timed by the
# caller; correctness = matching printed result.
#
#   BENCH=fib sh tools/rubybench.sh
#
# Env:
#   BENCH        benchmark name (benchmarks/<BENCH>.rb)   [required]
#   RB_MODE      plain (default) | aot | cruby | cruby-yjit | build
#   INTERP       sample binary   (default ./koruby_precise)
#   RUBY         reference ruby  (default ruby)
#   BENCH_ITRS   block iterations (default 1)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../apps/ruby-bench"
MODE=${RB_MODE:-plain}
INTERP=${INTERP:-./koruby_precise}
RUBY=${RUBY:-ruby}
: "${BENCH:?set BENCH=<name> (see apps/ruby-bench/benchmarks/*.rb)}"

[ -d "$APP/benchmarks" ] || sh "$HERE/rubybench_setup.sh"
# single-file micro (benchmarks/<name>.rb) or a self-contained app/CLBG bench
# (benchmarks/<name>/benchmark.rb — both just require loader + run_benchmark).
SRC="$APP/benchmarks/$BENCH.rb"
[ -f "$SRC" ] || SRC="$APP/benchmarks/$BENCH/benchmark.rb"
[ -f "$SRC" ] || { echo "no such bench: benchmarks/$BENCH(.rb | /benchmark.rb)"; exit 2; }
# Put the bundle BESIDE the source so __dir__-relative data files (e.g.
# blurhash/test.bin) resolve. The clone is gitignored, so this leaves no trace.
BUNDLE=${RB_BUNDLE:-"$(cd "$(dirname "$SRC")" && pwd)/.rubybench_$BENCH.rb"}
case "$INTERP" in /*|*" "*) ;; ./*) INTERP="$(cd "$(dirname "$INTERP")" && pwd)/$(basename "$INTERP")";; esac

# --- build the require-free bundle: koruby run_benchmark shim + the bench ---
{
  cat <<'SHIM'
BENCH_ITRS = (ENV["BENCH_ITRS"] ? ENV["BENCH_ITRS"].to_i : 1)
Random.srand(1337)                                 # harness-common: deterministic RNG
def make_shareable(obj, copy: false); obj; end     # harness-common fallback (no Ractor)
def run_benchmark(hint = 1, **opts)
  r = nil
  BENCH_ITRS.times { r = yield }
  p r
end
SHIM
  grep -vE "require_relative .*loader" "$SRC"
} > "$BUNDLE"

case "$MODE" in
  build)      echo "$BUNDLE"; exit 0 ;;
  cruby)      exec "$RUBY" --yjit-disable "$BUNDLE" ;;
  cruby-yjit) exec "$RUBY" --yjit "$BUNDLE" ;;
  aot)
    cd "$APP"; rm -rf code_store
    CCACHE_DISABLE=1 "$INTERP" --aot-compile "$BUNDLE" >/dev/null 2>&1 || { echo "aot-compile failed"; exit 1; }
    exec "$INTERP" --compiled-only "$BUNDLE" ;;
  plain|*)    exec "$INTERP" --plain "$BUNDLE" ;;
esac
