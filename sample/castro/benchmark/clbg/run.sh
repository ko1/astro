#!/bin/bash
# Run each adapted CLBG (Computer Language Benchmarks Game) kernel under
# castro AOT, gcc -O0, gcc -O3.  Reports median wall-clock + correctness.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
cd "$ROOT"

CASTRO=$ROOT/castro
[[ -x "$CASTRO" ]] || { echo "castro not built — run 'make' first." >&2; exit 1; }

KERNELS_DIR=$HERE/kernels
BIN_DIR=$HERE/bin
mkdir -p "$BIN_DIR"

export CCACHE_DIR=$ROOT/tmp/ccache
export CCACHE_DISABLE=1
mkdir -p "$CCACHE_DIR"

if [[ $# -gt 0 ]]; then
    KERNELS="$@"
else
    KERNELS="$(ls "$KERNELS_DIR"/*.c | xargs -n1 basename | sed 's/\.c$//')"
fi

RUNS=${RUNS:-7}

for k in $KERNELS; do
    src=$KERNELS_DIR/$k.c
    [[ -f "$src" ]] || { echo "missing: $src" >&2; continue; }

    gcc -O0 -o "$BIN_DIR/${k}_O0" "$src" 2>/dev/null
    gcc -O3 -o "$BIN_DIR/${k}_O3" "$src" 2>/dev/null

    rm -rf code_store "$BIN_DIR/${k}_code_store"
    "$CASTRO" --compile-all "$src" >/dev/null 2>&1 || true
    if [[ ! -f code_store/all.so ]]; then
        echo "AOT build failed for $k" >&2
        continue
    fi
    cp -r code_store "$BIN_DIR/${k}_code_store"
done

printf '%-20s %10s %10s %10s %10s %8s %8s\n' \
    bench castro_ms O0_ms O3_ms vs-O3 castro_rc O3_rc

for k in $KERNELS; do
    src=$KERNELS_DIR/$k.c
    [[ -f "$src" ]] || continue
    [[ -d "$BIN_DIR/${k}_code_store" ]] || continue

    rm -rf code_store
    cp -r "$BIN_DIR/${k}_code_store" code_store

    cs_ms=$(for i in $(seq 1 $RUNS); do /usr/bin/time -f "%e" "$CASTRO" -q "$src" 2>&1 | tail -1; done | sort -n | sed -n "$(( (RUNS + 1) / 2 ))p" | awk '{printf "%.0f", $1*1000}')
    cs_rc=$("$CASTRO" -q "$src" 2>/dev/null; echo $?)

    o0_ms=$(for i in $(seq 1 $RUNS); do /usr/bin/time -f "%e" "$BIN_DIR/${k}_O0" 2>&1 | tail -1; done | sort -n | sed -n "$(( (RUNS + 1) / 2 ))p" | awk '{printf "%.0f", $1*1000}')

    o3_ms=$(for i in $(seq 1 $RUNS); do /usr/bin/time -f "%e" "$BIN_DIR/${k}_O3" 2>&1 | tail -1; done | sort -n | sed -n "$(( (RUNS + 1) / 2 ))p" | awk '{printf "%.0f", $1*1000}')
    o3_rc=$("$BIN_DIR/${k}_O3"; echo $?)

    ratio="--"
    if [[ "$o3_ms" -gt 0 && "$cs_ms" -gt 0 ]]; then
        ratio=$(awk -v c="$cs_ms" -v o="$o3_ms" 'BEGIN{ printf "%.2fx", c/o }')
    fi

    printf '%-20s %10s %10s %10s %10s %8s %8s\n' \
        "$k" "$cs_ms" "$o0_ms" "$o3_ms" "$ratio" "$cs_rc" "$o3_rc"
done
