#!/usr/bin/env bash
# Take top-N configs from a bench log and re-run with high reps + long
# frames for confident validation.
#
# Usage: tools/bench-validate.sh <log_in> <log_out> <top_n>
LOG_IN="$1"
LOG_OUT="${2:-tools/bench-results/validate.log}"
TOP_N="${3:-10}"
RUNS="${RUNS:-10}"
FRAMES="${FRAMES:-600}"

cd /home/ko1/ruby/astro/sample/koruby

# Extract top-N configs from LOG_IN by best fps (deduped).
tmpcfg=$(mktemp)
bash tools/bench-summary.sh "$LOG_IN" 2>/dev/null | head -n "$TOP_N" | \
while read line; do
  id=$(echo "$line" | awk '{print $1}')
  flags=$(echo "$line" | sed -n 's/.*flags=//p' | sed 's/^a=//')
  # Extract k_cc / a_cc — we'll re-derive from a guess (id contains compiler).
  cc=$(echo "$id" | grep -oE 'gcc-[0-9]+|clang-[0-9]+')
  [ -z "$cc" ] && cc="gcc-15"  # fallback
  # Strip trailing backslash that came from %q quoting.
  flags=$(echo "$flags" | sed 's/\\$//;s/\\ / /g')
  echo "VAL_${id}|${cc}|${cc}|-O3|${flags}"
done > "$tmpcfg"

cat "$tmpcfg"
echo "---"
RUNS=$RUNS FRAMES=$FRAMES bash tools/bench-matrix.sh "$LOG_OUT" < "$tmpcfg"
rm -f "$tmpcfg"
