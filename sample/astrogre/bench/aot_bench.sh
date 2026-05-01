#!/usr/bin/env bash
# Bench harness aimed at *AOT-favourable* patterns: long node chain,
# no fixed first byte (prefilter doesn't fire), enough per-position
# work that the dispatch chain is a meaningful share of wall time.
#
# Drives the in-engine --bench-file path so the result isn't masked
# by the grep CLI's per-line getline / CTX-init overhead — full
# 118 MB corpus is read into memory once and the search runs
# repeatedly over it.
#
# Usage: ./aot_bench.sh [N]  (default N=3)

set -u
cd "$(dirname "$0")"

CORPUS="${CORPUS:-corpus_big.txt}"
[ -f "$CORPUS" ] || { echo "missing $CORPUS"; exit 1; }
LINES=$(wc -l < "$CORPUS")
BYTES=$(wc -c < "$CORPUS")

N=${1:-3}
ASTROGRE=../astrogre
[ -x "$ASTROGRE" ] || { echo "build ../astrogre first"; exit 1; }

extract_per() {
    awk '/per=/ { for (i = 1; i <= NF; i++) if ($i ~ /^per=[0-9.]+ms$/) { sub("per=", "", $i); sub("ms", "", $i); print $i; exit } }'
}

run_one() {
    local label="$1"; local pat="$2"
    local interp aot ratio
    rm -rf code_store
    interp=$("$ASTROGRE" --bench-file "$CORPUS" "$pat" "$N" --plain 2>&1 | extract_per)
    rm -rf code_store
    aot=$("$ASTROGRE" --bench-file "$CORPUS" "$pat" "$N" --aot 2>&1 | extract_per)
    ratio=$(awk -v a="$interp" -v b="$aot" 'BEGIN{ if (b > 0) printf "%.2fx", a/b; else print "n/a"; }')
    printf '  %-44s  interp %8s ms   aot %8s ms   %s\n' "$label" "$interp" "$aot" "$ratio"
}

echo "corpus: $CORPUS  ($LINES lines, $BYTES bytes)  best of $N runs"
echo "Patterns chosen so the prefilter does NOT fire (no fixed first byte)"
echo "and the chain is long enough that per-iter dispatch matters."
echo

# --- Backtrack-heavy alt+rep over a never-matching prefix ---
# Forces a full sweep of the 118 MB corpus and runs the chain at
# every position.  Showcases AOT specialisation.
run_one '/(QQQ|RRR)+\d+/'                    '/(QQQ|RRR)+\d+/'
run_one '/(QQQX|RRRX|SSSX)+/'                '/(QQQX|RRRX|SSSX)+/'
run_one '/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/' '/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/'

# --- Mixed class + literal + rep, sometimes match ---
run_one '/[a-z][0-9][a-z][0-9][a-z]/'        '/[a-z][0-9][a-z][0-9][a-z]/'
run_one '/(\d+\.\d+\.\d+\.\d+)/'             '/(\d+\.\d+\.\d+\.\d+)/'

# --- Long fixed class (no prefilter, but the inner work is small) ---
run_one '/[A-Z]{50,}/  (no match expected)'  '/[A-Z]{50,}/'
run_one '/[a-z]+\d+[a-z]+/'                  '/[a-z]+\d+[a-z]+/'

# --- Heavy capture + greedy ---
run_one '/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/' '/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/'
echo
