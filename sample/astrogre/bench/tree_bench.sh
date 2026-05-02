#!/usr/bin/env bash
# Recursive tree-walk bench — what `are` / `rg` / `grep -r` actually
# do day-to-day on a code tree.  Picks a few targets that are usually
# present on a dev machine; skips silently if a target's missing.
#
# Walker concerns dominate here — .gitignore parsing, parallel
# directory walking, per-file open/read overhead — not the regex
# engine itself.  Match counts are sanity-checked between tools.
#
# Usage: ./tree_bench.sh [N]   (N = best-of, default 3)

set -u
cd "$(dirname "$0")"

ARE=../are/are
GREP=$(command -v grep)

# rg may be a Claude Code shell wrapper — find the real binary.
RG=""
for cand in /usr/bin/rg /usr/local/bin/rg \
            /home/ko1/.vscode-server/extensions/github.copilot-chat-*/node_modules/@github/copilot/sdk/ripgrep/bin/linux-x64/rg \
            /home/ko1/.vscode-server/cli/servers/Stable-*/server/node_modules/@vscode/ripgrep/bin/rg
do
    if [ -x "$cand" ]; then RG="$cand"; break; fi
done

[ -x "$ARE" ] || { echo "build ../are/are first (cd ../are && make)"; exit 1; }

N=${1:-3}

# Best-of-N timer.  Accepts exit 0 or 1 as success (grep returns 1 for
# "no matches"); anything else gets recorded as ERR.
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

# `pat` is a Ruby/PCRE-style alternation.  `tree` is a directory.
# `t_short` is a short label for the type filter (rg's -t and our
# -t use the same names — c, ruby, py, etc.).
run_tree() {
    local label="$1" pat="$2" tree="$3" t_short="$4"
    [ -d "$tree" ] || { echo; echo "[$label] (skip — no $tree)"; return; }
    echo
    echo "[$label] /$pat/  in $tree  (-t $t_short)"
    best_of "are -j4"      "$ARE"  -j 4 -c -t "$t_short" -- "$pat" "$tree"
    if [ -n "$RG" ]; then
        best_of "ripgrep"  "$RG"   -c -t"$t_short"        -- "$pat" "$tree"
    fi
    case "$t_short" in
        c)    INCL=(--include='*.c' --include='*.h') ;;
        ruby) INCL=(--include='*.rb' --include='Rakefile' --include='Gemfile') ;;
        py)   INCL=(--include='*.py') ;;
        *)    INCL=() ;;
    esac
    best_of "grep -r"      "$GREP" -rcE "${INCL[@]}"     -- "$pat" "$tree"
}

echo "best of $N runs · -j 4 for are · same -t LANG / --include for grep"

# Single-literal: walker dominates.
run_tree "1lit usr/include" \
    "CONFIG"   /usr/include   c

# Multi-literal alt that fits in byteset (≤ 8 first bytes).
run_tree "3lit byteset"      \
    "PROT_READ|PROT_WRITE|MAP_PRIVATE"   /usr/include   c

# Multi-literal alt that needs AC (> 8 distinct first bytes).
run_tree "12lit AC"          \
    "PROT_READ|PROT_WRITE|MAP_PRIVATE|MAP_SHARED|S_ISREG|S_ISDIR|EBADF|EAGAIN|EBUSY|EINTR|ENOMEM|EINVAL" \
    /usr/include   c

# Astro tree — small post-gitignore, exercises the .gitignore walker.
run_tree "astro tree verbose_mark" \
    "verbose_mark"   ../..   c

echo
