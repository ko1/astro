#!/bin/sh
# json_parse.sh — JSON parse-throughput benchmark (ruby/ruby-bench
# "json_parse_float" family, minus the Ractor harness).  Generates ELEMENTS JSON
# documents (arrays of 20 floats, seeded RNG) and parses them all, checksumming
# the parsed float VALUES by their IEEE-754 bits (pack("E*").sum(64)).  Bits, not
# text: CRuby's C json gem prints floats to fuller precision than Ruby's
# Float#to_s (which koruby's lib/json.rb uses), so a string round-trip would
# differ — but both decode to the same doubles, so the bit checksum agrees.
# Exercises lib/json.rb Float parsing at scale.
#
# Env:
#   RB_MODE    plain (default) | aot | cruby | cruby-yjit | build
#   INTERP     sample binary   (default ./koruby_precise)
#   RUBY       reference ruby  (default ruby)
#   ELEMENTS   documents to parse (default 20000)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../apps/ruby-bench/benchmarks/json_parse_float"
MODE=${RB_MODE:-plain}
INTERP=${INTERP:-./koruby_precise}
RUBY=${RUBY:-ruby}

[ -d "$APP" ] || sh "$HERE/rubybench_setup.sh"
[ -d "$APP" ] || { echo "json_parse_float dir missing"; exit 2; }
case "$INTERP" in /*|*" "*) ;; ./*) INTERP="$(cd "$(dirname "$INTERP")" && pwd)/$(basename "$INTERP")";; esac

DRV="$APP/json_parse_headless.rb"
cat > "$DRV" <<'RB'
require "json"
Random.srand(1337)
n = (ENV["ELEMENTS"] || 20000).to_i
list = n.times.map { Array.new(20) { rand }.to_json }
sum = 0
list.each { |j| sum = (sum + JSON.parse(j).pack("E*").sum(64)) & 0xffffffffffffffff }
puts "elements=#{n} checksum=#{sum}"
RB

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
