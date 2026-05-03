#\!/usr/bin/env bash
# Print top-N results from a bench-matrix log, sorted by best fps.
LOG="${1:-tools/bench-results/phase1.log}"
N="${2:-10}"
echo "Top $N by best fps:"
grep -E "^P[0-9]" "$LOG" | sort -t= -k4 -nr | head -n "$N" | \
  awk '{ for (i=1;i<=NF;i++) printf "%s ", $i; print "" }'
echo ""
echo "Top $N by median fps:"
grep -E "^P[0-9]" "$LOG" | sort -t= -k5 -nr | head -n "$N"
