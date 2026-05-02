#!/usr/bin/env bash
# Compare `are` (production CLI on the astrogre engine) against
# grep -E and ripgrep on a fixed single-file corpus + a small set
# of representative patterns.  Each tool's output is sent to
# /dev/null and wall-clock time is taken with date +%s.%N.
#
# Engine-internal variants (astrogre interp / aot / +onigmo) live in
# aot_bench.sh — that's the engine perf rig.  This bench is for
# the user-facing CLI comparison.
#
# Best-of-N seconds (default N=3) — keeps the table to a sustained
# ~1 s scale so first-run setup doesn't dominate.
#
# Usage: ./grep_bench.sh [N]

set -u
cd "$(dirname "$0")"

# Pick the larger corpus if it's been generated, else fall back.
if [ -f corpus_big.txt ]; then CORPUS=corpus_big.txt
elif [ -f corpus.txt ];     then CORPUS=corpus.txt
else echo "no corpus — generate corpus.txt or corpus_big.txt"; exit 1
fi
LINES=$(wc -l <"$CORPUS")
BYTES=$(wc -c <"$CORPUS")

N=${1:-3}

ARE=../are/are
GREP=$(command -v grep)

# `rg` may be a shell wrapper for Claude Code in the user's profile;
# look for a real binary instead.
RG=""
for cand in /usr/bin/rg /usr/local/bin/rg \
            /home/ko1/.vscode-server/extensions/github.copilot-chat-*/node_modules/@github/copilot/sdk/ripgrep/bin/linux-x64/rg \
            /home/ko1/.vscode-server/cli/servers/Stable-*/server/node_modules/@vscode/ripgrep/bin/rg
do
    if [ -x "$cand" ]; then RG="$cand"; break; fi
done

[ -x "$ARE" ] || { echo "build ../are/are first (cd ../are && make)"; exit 1; }

# Best of N — accept exit 0 (matches) or 1 (no matches) as success;
# anything else is a real failure and we record "ERR".
best_of() {
    local tag="$1"; shift
    local best="999"
    local ok_any=0
    for ((i = 0; i < N; i++)); do
        local s e t rc=0
        s=$(date +%s.%N)
        "$@" >/dev/null 2>/dev/null
        rc=$?
        e=$(date +%s.%N)
        if [ "$rc" -le 1 ]; then
            ok_any=1
            t=$(awk -v a="$s" -v b="$e" 'BEGIN{print b-a}')
            best=$(awk -v a="$best" -v b="$t" 'BEGIN{print (b<a)?b:a}')
        fi
    done
    if [ "$ok_any" -eq 0 ]; then
        printf '  %-22s ERR\n' "$tag"
    else
        printf '  %-22s %.3f s\n' "$tag" "$best"
    fi
}

run_pattern() {
    local label="$1"
    local pat="$2"
    shift 2
    local extra=("$@")          # extra grep-style flags applied to all tools
    echo
    echo "[$label] /$pat/  ${extra[*]:-}"
    best_of "grep -E"        "$GREP"  -E "${extra[@]}" "$pat" "$CORPUS"
    if [ -n "$RG" ]; then
        best_of "ripgrep"    "$RG"    -j1 "${extra[@]}" -e "$pat" "$CORPUS"
    fi
    best_of "are -j1"        "$ARE"   -j 1 "${extra[@]}" -e "$pat" "$CORPUS"
}

echo "corpus: $CORPUS  ($LINES lines, $BYTES bytes)  best of $N runs"

run_pattern "literal"        "static"
run_pattern "literal-rare"   "specialized_dispatcher"
run_pattern "anchored"       "^static"
run_pattern "case-insens"    "VALUE"                  -i
run_pattern "alt-3"          "static|extern|inline"
# alt-12: stresses the AC prefilter (>8 distinct first bytes).
run_pattern "alt-12"         "static|extern|inline|return|while|switch|break|case|goto|asm|defined|sizeof"
run_pattern "class-rep"      "[0-9]{4,}"
run_pattern "ident-call"     "[a-z_]+_[a-z]+\("
run_pattern "count"          "static"                 -c
echo
