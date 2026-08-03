#!/bin/sh
# ruby_json.sh — pure-Ruby JSON parser benchmark (ruby/ruby-bench "ruby-json":
# a StringScanner-based JSON parser, no C json extension).  Parses a fixed data
# file (data.json — public-domain football data) a fixed number of times and
# prints a checksum of the parsed structure (canonicalised via JSON.generate).
# Deterministic: CRuby and an ASTro sample must agree.  Exercises koruby's
# lib/strscan.rb + lib/json.rb + Regexp captures over an ASCII-8BIT subject.
#
# Env:
#   RB_MODE   plain (default) | aot | cruby | cruby-yjit | build
#   INTERP    sample binary   (default ./koruby_precise)
#   RUBY      reference ruby  (default ruby)
#   ITRS      parse iterations (default 200)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../apps/ruby-bench/benchmarks/ruby-json"
MODE=${RB_MODE:-plain}
INTERP=${INTERP:-./koruby_precise}
RUBY=${RUBY:-ruby}

[ -f "$APP/benchmark.rb" ] || sh "$HERE/rubybench_setup.sh"
[ -f "$APP/data.json" ] || { echo "ruby-json data.json missing"; exit 2; }

case "$INTERP" in /*|*" "*) ;; ./*) INTERP="$(cd "$(dirname "$INTERP")" && pwd)/$(basename "$INTERP")";; esac

# Driver = the benchmark's JSONParser (loader/SOURCE/run_benchmark stripped) +
# a deterministic parse-and-checksum body.  Written beside the source so
# __dir__/data.json resolves; the clone is gitignored so this leaves no trace.
DRV="$APP/ruby_json_headless.rb"
{
  grep -vE "require_relative .*loader|^SOURCE =|^run_benchmark|IO\.read" "$APP/benchmark.rb"
  cat <<'BODY'
require "json"
SRC  = File.read(File.join(__dir__, "data.json"))
ITRS = (ENV["ITRS"] || 200).to_i
r = nil
ITRS.times { r = JSONParser.new(SRC.dup).parse }
h = 14695981039346656037
JSON.generate(r).each_byte { |b| h = ((h ^ b) * 1099511628211) & 0xffffffffffffffff }
puts "itrs=#{ITRS} checksum=#{h}"
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
