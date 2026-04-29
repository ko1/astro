#!/usr/bin/env bash
# Compare ascheme (interpreter + AOT) against chibi-scheme on the
# benchmarks under bench/.  chibi-scheme is fetched + built on first
# run into ../.chibi/ — re-uses the local build afterwards.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHIBI_DIR="$ROOT/.chibi"
CHIBI_BIN="$CHIBI_DIR/chibi-scheme-0.12/chibi-scheme"
ASCHEME="$ROOT/ascheme"
BENCH_DIR="$ROOT/bench"

build_chibi() {
    if [ -x "$CHIBI_BIN" ]; then return 0; fi
    echo "Fetching + building chibi-scheme into $CHIBI_DIR ..."
    mkdir -p "$CHIBI_DIR"
    local TGZ="$CHIBI_DIR/chibi-0.12.tar.gz"
    curl -sL https://github.com/ashinn/chibi-scheme/archive/refs/tags/0.12.tar.gz -o "$TGZ"
    tar xf "$TGZ" -C "$CHIBI_DIR"
    pushd "$CHIBI_DIR/chibi-scheme-0.12" >/dev/null
    CCACHE_DISABLE=1 make -j"$(nproc)" >/dev/null 2>&1
    popd >/dev/null
}

run_chibi() {
    LD_LIBRARY_PATH="$CHIBI_DIR/chibi-scheme-0.12" \
    CHIBI_MODULE_PATH="$CHIBI_DIR/chibi-scheme-0.12/lib" \
    "$CHIBI_BIN" "$@"
}

time_run() {
    # POSIX `time` writes to stderr; we capture and extract real seconds.
    local out
    out=$( { /usr/bin/time -f "%e" "$@" >/dev/null; } 2>&1 )
    printf "%s" "$out"
}

build_chibi

if [ ! -x "$ASCHEME" ]; then
    echo "ascheme binary not found — running 'make' first" >&2
    (cd "$ROOT" && make >/dev/null)
fi

CHIBI_RUN=( env LD_LIBRARY_PATH="$CHIBI_DIR/chibi-scheme-0.12"
            "$CHIBI_DIR/chibi-scheme-0.12/chibi-scheme"
            -A"$CHIBI_DIR/chibi-scheme-0.12/lib"
            -m '(scheme base)' -m '(scheme write)' )

# Guile: a heavyweight optimizer + JIT.  Pre-compiling to .go would
# inflate first-run timing, so we disable auto-compile and run the
# source directly — matches what we do for the other implementations.
GUILE_AVAILABLE=0
if command -v guile >/dev/null; then GUILE_AVAILABLE=1; fi
GUILE_RUN=( env GUILE_AUTO_COMPILE=0 guile -s )

# Two AOT measurements:
#   aot-first:  --clear-cs forces SD_*.c regeneration + cc + link before running.
#               Reflects the cost a user pays the first time they hit a cold
#               code store.
#   aot-cached: code_store/ already populated.  astro_cs_compile sees existing
#               .c files, make sees up-to-date .o files, so the build phase
#               is just a near-no-op `make` invocation before evaluating.
# Default: run only the small set.  `bench/compare.sh big` runs the
# heavy set (multi-second workloads), `bench/compare.sh all` does both.
SET="${1:-small}"

declare -a SETS
case "$SET" in
    small)  SETS=( small ) ;;
    big)    SETS=( big ) ;;
    all)    SETS=( small big ) ;;
    *)      echo "usage: $0 [small|big|all]" >&2; exit 1 ;;
esac

for s in "${SETS[@]}"; do
    echo
    echo "=== bench/$s/ ==="
    if [ "$GUILE_AVAILABLE" = "1" ]; then
        printf "%-12s %10s %10s %10s %10s %10s %10s %10s\n" "benchmark" "interp" "aot-first" "aot-cached" "pg-compile" "pg-cached" "chibi" "guile"
        printf "%-12s %10s %10s %10s %10s %10s %10s %10s\n" "---------" "------" "---------" "----------" "----------" "---------" "-----" "-----"
    else
        printf "%-12s %10s %10s %10s %10s %10s %10s\n" "benchmark" "interp" "aot-first" "aot-cached" "pg-compile" "pg-cached" "chibi"
        printf "%-12s %10s %10s %10s %10s %10s %10s\n" "---------" "------" "---------" "----------" "----------" "---------" "-----"
    fi

    for b in "$BENCH_DIR/$s"/*.scm; do
        [ -f "$b" ] || continue
        name=$(basename "$b" .scm)

        t_interp=$(time_run "$ASCHEME" -q "$b")

        # Cold AOT: clear store first so we measure SD_*.c generation +
        # gcc + link + dlopen as part of the run.  CCACHE_DISABLE=1 is
        # set both here (visibility) and inside ascheme via setenv (so
        # the spawned `make` sub-shell honours it even if invoked some
        # other way) — without it, ccache caches .o per (cmd, mtime,
        # source hash) and a "cold" rebuild is anything but cold.
        t_aot_first=$(CCACHE_DISABLE=1 time_run "$ASCHEME" --clear-cs -q -c "$b")

        # Warm AOT: code_store/ from `aot-first` above is reused; astro_cs
        # finds the .o files up to date and skips the rebuild.  This is
        # what subsequent invocations of an AOT-compiled program see.
        t_aot_cached=$(time_run "$ASCHEME" -q -c "$b")

        # PGO compile (`--pg-compile`, abruby-style): single invocation,
        # interpret + post-execution AOT compile of hot entries only.
        # Wipes the code_store first to force a cold compile so the
        # measurement reflects the full first-time cost.
        t_pg_compile=$(CCACHE_DISABLE=1 time_run "$ASCHEME" --clear-cs -q --pg-compile "$b")

        # PGO cached: the *next* invocation, which picks up only the
        # hot SDs that `--pg-compile` baked.  Smaller code_store than
        # aot-cached since cold entries are absent.
        t_pg_cached=$(time_run "$ASCHEME" -q -c "$b")

        t_chibi=$(time_run "${CHIBI_RUN[@]}" "$b")

        if [ "$GUILE_AVAILABLE" = "1" ]; then
            t_guile=$(time_run "${GUILE_RUN[@]}" "$b")
            printf "%-12s %10s %10s %10s %10s %10s %10s %10s\n" "$name" \
                "${t_interp}s" "${t_aot_first}s" "${t_aot_cached}s" \
                "${t_pg_compile}s" "${t_pg_cached}s" \
                "${t_chibi}s" "${t_guile}s"
        else
            printf "%-12s %10s %10s %10s %10s %10s %10s\n" "$name" \
                "${t_interp}s" "${t_aot_first}s" "${t_aot_cached}s" \
                "${t_pg_compile}s" "${t_pg_cached}s" "${t_chibi}s"
        fi
    done
done
