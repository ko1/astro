#!/bin/sh
# etanni.sh — Etanni template-engine benchmark (ruby/ruby-bench "etanni": an
# eval-based ERB-style template compiled to a Proc, rendered against gem-server
# data loaded from JSON).  Renders a fixed template ITRS times and prints a
# checksum of the output (String#sum(64)) — deterministic, CRuby and an ASTro
# sample must agree.  Exercises eval→Proc + instance_eval + heredoc templates +
# koruby's lib/json.rb (JSON.load of a 343 KB gem_specs.json).
#
# Env:
#   RB_MODE   plain (default) | aot | cruby | cruby-yjit | build
#   INTERP    sample binary   (default ./koruby_precise)
#   RUBY      reference ruby  (default ruby)
#   ITRS      render iterations (default 100)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../apps/ruby-bench/benchmarks/etanni"
MODE=${RB_MODE:-plain}
INTERP=${INTERP:-./koruby_precise}
RUBY=${RUBY:-ruby}

[ -f "$APP/benchmark.rb" ] || sh "$HERE/rubybench_setup.sh"
[ -f "$APP/simple_template.etanni" ] || { echo "etanni template missing"; exit 2; }

case "$INTERP" in /*|*" "*) ;; ./*) INTERP="$(cd "$(dirname "$INTERP")" && pwd)/$(basename "$INTERP")";; esac

# Driver = the benchmark's setup (class + JSON.load + compile), loader/run_benchmark
# stripped, plus a deterministic render-and-checksum body.
DRV="$APP/etanni_headless.rb"
{
  sed '/^run_benchmark/,$d' "$APP/benchmark.rb" | grep -vE "require_relative .*loader"
  cat <<'BODY'
ITRS = (ENV["ITRS"] || 100).to_i
r = nil
ITRS.times { r = run_etanni }
puts "itrs=#{ITRS} bytes=#{r.bytesize} checksum=#{r.sum(64)}"
BODY
} > "$DRV"

case "$MODE" in
  build)      echo "$DRV"; exit 0 ;;
  cruby)      exec "$RUBY" --yjit-disable "$DRV" ;;
  cruby-yjit) exec "$RUBY" --yjit "$DRV" ;;
  aot)
    cd "$APP"; rm -rf code_store
    CCACHE_DISABLE=1 "$INTERP" --aot-compile --run "$DRV" >/dev/null 2>&1 || { echo "aot-compile failed"; exit 1; }
    exec "$INTERP" --compiled-only "$DRV" ;;
  plain|*)    exec "$INTERP" --plain "$DRV" ;;
esac
