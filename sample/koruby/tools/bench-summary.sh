#!/usr/bin/env bash
# Summarize bench-matrix logs.
LOGS="$@"
[ -z "$LOGS" ] && LOGS=$(echo tools/bench-results/*.log)

{
  for log in $LOGS; do
    grep -E "^[A-Z][A-Z0-9]*_" "$log" 2>/dev/null | grep -v "BUILD_FAIL\|NO_FPS\|NO_GCDA\|^=="
  done
} | awk '
{
  id = $1
  for (i=1;i<=NF;i++) {
    if ($i ~ /^best=/)   best = substr($i,6)
    if ($i ~ /^median=/) median = substr($i,8)
    if ($i ~ /^mean=/)   mean = substr($i,6)
    if ($i ~ /^size=/)   sz = substr($i,6)
  }
  flags = ""
  for (i=1;i<=NF;i++) if ($i ~ /^a=/) flags = $i
  key = id "|" sz "|" flags
  if (!(key in best_seen) || best+0 > best_seen[key]+0) {
    best_seen[key] = best; median_seen[key] = median; mean_seen[key] = mean
  }
}
END {
  for (k in best_seen) {
    split(k, p, "|")
    printf "%-50s size=%-8d best=%-8.2f median=%-8.2f mean=%-8.2f flags=%s\n", \
      p[1], p[2], best_seen[k], median_seen[k], mean_seen[k], p[3]
  }
}
' | sort -k4 -t= -nr
