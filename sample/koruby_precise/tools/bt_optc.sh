#!/usr/bin/env bash
# optcarrot 180-frame instruction count for the frame-layout A/B.
#   tools/bt_optc.sh <tag>
# Runs entirely inside $OUT (its own bundle + code_store), so the shared
# sample/abruby/benchmark/optcarrot tree is never written to.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
TAG=${1:?usage: bt_optc.sh <tag>}
FRAMES=${FRAMES:-180}
OUT=${OUT:-/tmp/claude-1000/-home-ko1-ruby-astro/0a48b788-4b01-46f0-a6f8-490a93bf91eb/scratchpad/bt}
OPT="$HERE/../abruby/benchmark/optcarrot"
W="$OUT/optc"
mkdir -p "$W"
ln -sfn "$OPT/examples" "$W/examples"
BUNDLE="$W/bundle.rb"
OPTC_MODE=build "$HERE/tools/optcarrot.sh" "$FRAMES" >/dev/null
cp /tmp/optc_bundle.rb "$BUNDLE"
cd "$W" || exit 1
rm -rf code_store
CCACHE_DISABLE=1 "$HERE/koruby_precise" --aot-compile "$BUNDLE" >/dev/null 2>"$OUT/$TAG.optc.compile.log" || { echo "optcarrot COMPILE-FAIL"; exit 1; }
txt=$(size -A code_store/op/*.o 2>/dev/null | awk '$1==".text"{s+=$2} END{print s+0}')
"$HERE/koruby_precise" --compiled-only "$BUNDLE" > "$OUT/$TAG.optc.run" 2>&1
perf stat -r 3 -e instructions,cycles -x, -o "$OUT/$TAG.optc.perf" "$HERE/koruby_precise" --compiled-only "$BUNDLE" >/dev/null 2>&1
ins=$(awk -F, '$3=="instructions"{print $1}' "$OUT/$TAG.optc.perf")
cyc=$(awk -F, '$3=="cycles"{print $1}' "$OUT/$TAG.optc.perf")
echo "optcarrot${FRAMES} instructions=$ins cycles=$cyc text=$txt"
grep -iE "checksum|fps" "$OUT/$TAG.optc.run" | head -5
