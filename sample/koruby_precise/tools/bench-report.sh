#!/usr/bin/env bash
# Combined report from all bench logs.
cd /home/ko1/ruby/astro/sample/koruby
LOGS="$(echo tools/bench-results/*.log)"

echo "================================================================"
echo "  koruby + optcarrot bench report"
echo "  date: $(date -Iseconds)"
echo "  logs: $LOGS"
echo "================================================================"
echo ""

echo "Top 30 by best fps (deduped, all phases combined):"
echo "----"
bash tools/bench-summary.sh $LOGS | head -30

echo ""
echo "================================================================"
echo "Top 5 by median fps:"
echo "----"
bash tools/bench-summary.sh $LOGS | sort -k7 -t= -nr | head -5

echo ""
echo "================================================================"
echo "Total configs benched: $(bash tools/bench-summary.sh $LOGS | wc -l)"
