#!/usr/bin/env bash
# Whole-engine comparison rig.  This is the ASTro framework's
# experimental result table — we want to see plain (interp) vs
# AOT-first (cold compile) vs AOT-cached (warm load) vs the legacy
# Onigmo backend, side by side with grep / ripgrep, on the same
# corpus and patterns.
#
# Each row is a tool variant.  `aot-first` clears code_store/
# before each timed run so it includes the cs_compile + cs_build
# (cc invocation) cost.  `aot-cached` warms once then times the
# load-only path.
#
# Usage: ./grep_bench.sh [N]   (best-of-N, default N=3)
# Env:   RAW=/path/to/results.tsv  → also emit per-run TSV
# Env:   ASTROGRE=/alt/binary, ARE=/alt/are/are
#        (defaults: ../astrogre, ../are/are)

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

# Raw mode: emit one TSV row per individual run (not best-of) so
# callers can compute their own statistics, plot variance, etc.
# RAW=/path/to/file enables this; the file gets:
#   tool<TAB>label<TAB>pattern<TAB>run_idx<TAB>seconds
# rows.  The human-readable best-of summary still goes to stdout.
RAW="${RAW:-}"

ASTROGRE="${ASTROGRE:-../astrogre}"
ARE="${ARE:-../are/are}"

# astrogre's `astro_cs` creates `code_store/` relative to the
# launching process's cwd.  This script cd'd into bench/, so the
# cache lands at bench/code_store/.  We blow it away before each
# AOT-first timed run to include the cs_compile + cs_build cost.
CODE_STORE="${CODE_STORE:-code_store}"

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

[ -x "$ASTROGRE" ] || { echo "build ../astrogre first (cd .. && make)"; exit 1; }
[ -x "$ARE" ]      || echo "(no $ARE — skipping the are -j1 row)"

# The AOT compiler's cc invocation needs ccache off for the sandboxed
# build dir to work; the framework already passes CCACHE_DISABLE=1
# inside its make call but we belt-and-brace here too.
export CCACHE_DISABLE=1

# Best of N — accept exit 0 (matches) or 1 (no matches) as success;
# anything else is a real failure and we record "ERR".  When RAW is
# set, each individual measurement is appended as a TSV row to the
# file so callers can compute their own statistics.
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
            if [ -n "$RAW" ]; then
                printf '%s\t%s\t%s\t%d\t%s\n' "$tag" "$RAW_LABEL" "$RAW_PAT" "$i" "$t" >>"$RAW"
            fi
        elif [ -n "$RAW" ]; then
            printf '%s\t%s\t%s\t%d\tERR\n' "$tag" "$RAW_LABEL" "$RAW_PAT" "$i" >>"$RAW"
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
    RAW_LABEL="$label" RAW_PAT="$pat"
    echo
    echo "[$label] /$pat/  ${extra[*]:-}"
    best_of "grep -E"            "$GREP" -E "${extra[@]}" "$pat" "$CORPUS"
    if [ -n "$RG" ]; then
        best_of "ripgrep"        "$RG"   -j1 "${extra[@]}" -e "$pat" "$CORPUS"
    fi

    # ── ASTro engine variants (the actual experiment) ──────────
    # plain interp = no code-store load; pure AST dispatch.
    best_of "astrogre plain"     "$ASTROGRE" --plain "${extra[@]}" "$pat" "$CORPUS"

    # +onigmo: legacy Ruby regex engine baseline.  Skipped on
    # builds without WITH_ONIGMO=1.
    best_of "astrogre +onigmo"   "$ASTROGRE" --backend=onigmo "${extra[@]}" "$pat" "$CORPUS"

    # AOT-first: include cs_compile + cs_build (cc invocation) cost.
    # Each timed run starts with a clean code_store, so we measure
    # cold-cache compile-and-load.
    best_of_aot_first() {
        local tag="$1"; shift
        local best="999"
        local ok_any=0
        for ((i = 0; i < N; i++)); do
            rm -rf "$CODE_STORE"
            local s e t rc=0
            s=$(date +%s.%N)
            "$@" >/dev/null 2>/dev/null
            rc=$?
            e=$(date +%s.%N)
            if [ "$rc" -le 1 ]; then
                ok_any=1
                t=$(awk -v a="$s" -v b="$e" 'BEGIN{print b-a}')
                best=$(awk -v a="$best" -v b="$t" 'BEGIN{print (b<a)?b:a}')
                if [ -n "$RAW" ]; then
                    printf '%s\t%s\t%s\t%d\t%s\n' "$tag" "$RAW_LABEL" "$RAW_PAT" "$i" "$t" >>"$RAW"
                fi
            elif [ -n "$RAW" ]; then
                printf '%s\t%s\t%s\t%d\tERR\n' "$tag" "$RAW_LABEL" "$RAW_PAT" "$i" >>"$RAW"
            fi
        done
        if [ "$ok_any" -eq 0 ]; then
            printf '  %-22s ERR\n' "$tag"
        else
            printf '  %-22s %.3f s\n' "$tag" "$best"
        fi
    }
    best_of_aot_first "astrogre aot/first" \
        "$ASTROGRE" --aot-compile "${extra[@]}" "$pat" "$CORPUS"

    # AOT-cached: warm once then time load-only.
    "$ASTROGRE" --aot-compile "${extra[@]}" "$pat" "$CORPUS" >/dev/null 2>&1 || true
    best_of "astrogre aot/cached" "$ASTROGRE" --aot-compile "${extra[@]}" "$pat" "$CORPUS"

    # are: production CLI on the same engine, default = interp.
    if [ -x "$ARE" ]; then
        best_of "are -j1"        "$ARE" -j 1 "${extra[@]}" -e "$pat" "$CORPUS"
    fi
}

echo "corpus: $CORPUS  ($LINES lines, $BYTES bytes)  best of $N runs"
if [ -n "$RAW" ]; then
    : > "$RAW"
    printf 'tool\tlabel\tpattern\trun\tseconds\n' > "$RAW"
    echo "raw: $RAW"
fi

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
