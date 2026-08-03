#!/bin/sh
# knucleotide.sh — k-nucleotide benchmark (ruby/ruby-bench "knucleotide", a
# Computer-Language-Benchmarks-Game task: count DNA k-mer frequencies).  The
# upstream bench forks worker processes over IO.pipe; koruby has no fork/pipe,
# so this drives the pure-Ruby core (frequency / sort_by_freq / find_seq)
# single-threaded over the same deterministic 100 KB synthetic sequence, ITRS
# times, and prints a checksum of the results.  CRuby and an ASTro sample must
# agree.  Exercises Hash(default), String#byteslice, and heavy string/hash work.
#
# Env:
#   RB_MODE   plain (default) | aot | cruby | cruby-yjit | build
#   INTERP    sample binary   (default ./koruby_precise)
#   RUBY      reference ruby  (default ruby)
#   ITRS      iterations (default 5) ; SIZE  sequence length (default 100000)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../apps/ruby-bench/benchmarks/knucleotide"
MODE=${RB_MODE:-plain}
INTERP=${INTERP:-./koruby_precise}
RUBY=${RUBY:-ruby}

[ -f "$APP/benchmark.rb" ] || sh "$HERE/rubybench_setup.sh"
[ -f "$APP/benchmark.rb" ] || { echo "knucleotide benchmark.rb missing"; exit 2; }
case "$INTERP" in /*|*" "*) ;; ./*) INTERP="$(cd "$(dirname "$INTERP")" && pwd)/$(basename "$INTERP")";; esac

# Driver = the bench's pure-Ruby core (loader + fork/pipe run_benchmark stripped)
# + a single-threaded compute-and-checksum body.
DRV="$APP/knucleotide_headless.rb"
{
  sed '/^run_benchmark/,$d' "$APP/benchmark.rb" | grep -vE "require_relative .*loader"
  cat <<'BODY'
ITRS    = (ENV["ITRS"] || 5).to_i
freqs   = [1, 2]
nucleos = %w(GGT GGTA GGTATT GGTATTTTAATT GGTATTTTAATTTATAGT)
results = nil
ITRS.times do
  results = freqs.map { |i| sort_by_freq(TEST_SEQUENCE, i) } +
            nucleos.map { |s| find_seq(TEST_SEQUENCE, s) }
end
puts "itrs=#{ITRS} checksum=#{results.join("|").sum(64)}"
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
