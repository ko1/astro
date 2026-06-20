#!/usr/bin/env bash
# leak_check.sh — regression test for native-memory leaks (mpz limbs, class
# method tables, ...) that the diff harness can't see.
#
# Each scenario churns leak-prone objects (bignums, anonymous classes) and is run
# under BARUBY_GC_STRESS (GC every alloc → garbage is collected immediately, so a
# missing finalizer shows up as leaked blocks) inside valgrind.  We assert the
# "definitely lost" BLOCK count stays at/under THRESHOLD — the framework keeps a
# small, constant set of at-exit allocations (~5 blocks: finalize_list, GC arena
# metadata) that are never freed and are not a per-object leak.  A regression
# (e.g. dropping a finalizer) jumps this into the dozens/hundreds.
#
# Usage:  tools/leak_check.sh [./koruby_precise]
set -u
cd "$(dirname "$0")/.."
BIN="${1:-./koruby_precise}"
THRESHOLD=15            # framework at-exit overhead is ~5 blocks; a real leak is 50+

if ! command -v valgrind >/dev/null 2>&1; then
    echo "SKIP: valgrind not installed"; exit 0
fi
if [ ! -x "$BIN" ]; then
    echo "FAIL: $BIN not found/executable"; exit 1
fi

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# scenario name | ruby source (churns + drops leak-prone objects)
run_scenario() {
    local name="$1" src="$2"
    printf '%s\n' "$src" > "$tmp/$name.rb"
    local out blocks
    out="$(BARUBY_GC_STRESS=1 valgrind --leak-check=full --error-exitcode=0 \
           "$BIN" "$tmp/$name.rb" 2>&1)"
    # "definitely lost: 65,808 bytes in 5 blocks"
    blocks="$(printf '%s' "$out" | sed -n 's/.*definitely lost:.* in \([0-9,]*\) blocks.*/\1/p' | tr -d ',')"
    blocks="${blocks:-?}"
    if [ "$blocks" = "?" ]; then
        echo "FAIL  $name: could not parse valgrind output"; return 1
    fi
    if [ "$blocks" -le "$THRESHOLD" ]; then
        echo "PASS  $name: $blocks definitely-lost blocks (<= $THRESHOLD)"; return 0
    fi
    echo "FAIL  $name: $blocks definitely-lost blocks (> $THRESHOLD) — leak regression"; return 1
}

rc=0
run_scenario bignum_churn \
'sum = 0
2000.times { |i| x = (10 ** 400) + i; y = x * x; sum += y % 97 }
p sum' || rc=1

run_scenario class_churn \
'r = []
1000.times { |i| k = Class.new { def m1(x); x * 2; end; def m2; 7; end }; r << k.new.m1(i) if i % 400 == 0 }
p r.size' || rc=1

run_scenario struct_churn \
'n = 0
500.times { |i| s = Struct.new(:a, :b); n += s.new(i, i + 1).a }
p n' || rc=1

[ "$rc" -eq 0 ] && echo "leak_check: ALL PASS" || echo "leak_check: FAILURES"
exit "$rc"
