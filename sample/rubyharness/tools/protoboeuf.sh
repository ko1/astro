#!/bin/sh
# protoboeuf.sh — pure-Ruby Protocol Buffers benchmark (ruby/ruby-bench
# "protoboeuf": a generated pure-Ruby protobuf codec, no C ext).  Marshal-loads
# a fixed set of encoded messages, decodes them, then re-encodes them ITRS times
# and prints a checksum (String#sum(64) over the encoded bytes) — deterministic,
# CRuby and an ASTro sample must agree (decode + encode are byte-exact).
# Exercises Marshal.load, Array#pack(buffer:), String#<<(int) on ASCII-8BIT.
#
# Env:
#   RB_MODE   plain (default) | aot | cruby | cruby-yjit | build
#   INTERP    sample binary   (default ./koruby_precise)
#   RUBY      reference ruby  (default ruby)
#   ITRS      encode iterations (default 30)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../apps/ruby-bench/benchmarks/protoboeuf"
MODE=${RB_MODE:-plain}
INTERP=${INTERP:-./koruby_precise}
RUBY=${RUBY:-ruby}

[ -f "$APP/benchmark_pb.rb" ] || sh "$HERE/rubybench_setup.sh"
[ -f "$APP/encoded_msgs.bin" ] || { echo "protoboeuf data missing"; exit 2; }

case "$INTERP" in /*|*" "*) ;; ./*) INTERP="$(cd "$(dirname "$INTERP")" && pwd)/$(basename "$INTERP")";; esac

DRV="$APP/protoboeuf_headless.rb"
cat > "$DRV" <<'RB'
$LOAD_PATH.unshift(__dir__)
require_relative 'benchmark_pb'
Dir.chdir __dir__

bins = Marshal.load(File.binread('encoded_msgs.bin'))
lots = bins.map { |b| ProtoBoeuf::ParkingLot.decode(b) }
n    = (ENV['ITRS'] || 30).to_i

sum = 0
n.times do
  lots.each { |lot| sum = (sum + ProtoBoeuf::ParkingLot.encode(lot).sum(64)) & 0xffffffffffffffff }
end
puts "lots=#{lots.size} itrs=#{n} checksum=#{sum}"
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
