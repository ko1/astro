#!/usr/bin/env bash
# AOT-favourable bench harness — compares astrogre interp / aot
# against grep / ripgrep / onigmo on patterns where the chain is long
# enough that bake's dispatch elimination is worth measuring.
#
# astrogre uses --bench-file (whole 118 MB corpus loaded once,
# astrogre_search called N times) so the engine cost is isolated
# from grep CLI per-line getline / CTX-init overhead.  grep /
# ripgrep / onigmo run via the actual grep CLI in count mode (-c)
# — they walk the whole file in one go, which matches what
# --bench-file is doing (one full sweep per iter).
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

GREP=$(command -v grep)
RG=""
for cand in /usr/bin/rg /usr/local/bin/rg \
            /home/ko1/.vscode-server/cli/servers/Stable-*/server/node_modules/@vscode/ripgrep/bin/rg
do
    if [ -x "$cand" ]; then RG="$cand"; break; fi
done

extract_per() {
    awk '/per=/ { for (i = 1; i <= NF; i++) if ($i ~ /^per=[0-9.]+ms$/) { sub("per=", "", $i); sub("ms", "", $i); print $i; exit } }'
}

# best of N seconds → milliseconds, accepting grep's exit 1 (no
# matches) as success
best_of_ms() {
    local best=999000
    local ok=0
    for ((i = 0; i < N; i++)); do
        local s e t rc
        s=$(date +%s.%N)
        "$@" >/dev/null 2>/dev/null
        rc=$?
        e=$(date +%s.%N)
        if [ "$rc" -le 1 ]; then
            ok=1
            t=$(awk -v a="$s" -v b="$e" 'BEGIN{printf "%.3f", (b-a)*1000}')
            best=$(awk -v a="$best" -v b="$t" 'BEGIN{print (b<a)?b:a}')
        fi
    done
    if [ "$ok" -eq 0 ]; then echo "ERR"; else echo "$best"; fi
}

run_one() {
    local label="$1"
    local pat_lit="$2"     # /pat/ form (for astrogre and dump)
    local pat_re="$3"      # raw form (for grep -E)
    local interp aot ognm grep_t rg_t

    rm -rf code_store
    interp=$("$ASTROGRE" --bench-file "$CORPUS" "$pat_lit" "$N" --plain 2>&1 | extract_per)
    rm -rf code_store
    aot=$("$ASTROGRE" --bench-file "$CORPUS" "$pat_lit" "$N" --aot 2>&1 | extract_per)

    # Onigmo via grep CLI -c: walks the whole file counting matches.
    # Different semantic from "find first" but for these mostly-no-match
    # patterns the work is similar (full sweep).
    ognm=$(best_of_ms "$ASTROGRE" --backend=onigmo -c "$pat_re" "$CORPUS")
    grep_t=$(best_of_ms "$GREP"      -E -c "$pat_re" "$CORPUS")
    if [ -n "$RG" ]; then
        rg_t=$(best_of_ms "$RG" --no-mmap -j1 -c -e "$pat_re" "$CORPUS")
    else
        rg_t="-"
    fi

    local ratio_aot ratio_aot_vs_ognm
    ratio_aot=$(awk -v a="$interp" -v b="$aot" 'BEGIN{ if (b > 0) printf "%.2f", a/b; else print "n/a"; }')
    ratio_aot_vs_ognm=$(awk -v a="$aot" -v b="$ognm" 'BEGIN{ if (b > 0 && b != "ERR") printf "%.2f", a/b; else print "n/a"; }')

    printf '  %-44s  interp %8s  aot %8s (%5sx)  +onigmo %8s  grep %8s  ripgrep %8s\n' \
        "$label" "$interp" "$aot" "$ratio_aot" "$ognm" "$grep_t" "$rg_t"
}

echo "corpus: $CORPUS  ($LINES lines, $BYTES bytes)  best of $N runs   (all ms)"
echo "Patterns chosen so astrogre's prefilter does NOT fire and the chain is long"
echo "enough that per-iter dispatch matters.  astrogre uses --bench-file (single"
echo "buffer, no per-line overhead); grep/rg/onigmo via -c (count, full file sweep)."
echo

run_one '/(QQQ|RRR)+\d+/'                       '/(QQQ|RRR)+\d+/'                  '(QQQ|RRR)+[0-9]+'
run_one '/(QQQX|RRRX|SSSX)+/'                   '/(QQQX|RRRX|SSSX)+/'              '(QQQX|RRRX|SSSX)+'
run_one '/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/'   '/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/' '[a-z][0-9][A-Z][0-9][a-z][0-9][A-Z][0-9][a-z]'
run_one '/[a-z][0-9][a-z][0-9][a-z]/'           '/[a-z][0-9][a-z][0-9][a-z]/'      '[a-z][0-9][a-z][0-9][a-z]'
run_one '/(\d+\.\d+\.\d+\.\d+)/'                '/(\d+\.\d+\.\d+\.\d+)/'           '([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)'
run_one '/[A-Z]{50,}/'                          '/[A-Z]{50,}/'                     '[A-Z]{50,}'
run_one '/\b(if|else|for|while|return)\b/'      '/\b(if|else|for|while|return)\b/' '\b(if|else|for|while|return)\b'
run_one '/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/'    '/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/' '(\w+)[[:space:]]*\([[:space:]]*(\w+)[[:space:]]*,[[:space:]]*(\w+)\)'
echo
