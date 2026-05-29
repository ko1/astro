#!/bin/bash
# Sample sweep across core spec files (one per category).  Outputs:
# PASS=<n> FAIL=<n> ERR=<n> CRASH=<n> PROCESSED=<n>
# Usage: tools/spec_sweep.sh [SPEC_DIR] [CATEGORY_LIMIT_PER_DIR]
# Default: scans 150 spec files across enumerable/array/hash/string/etc.

cd "$(dirname "$0")/.."
SPEC_BASE=${1:-$HOME/ruby/src/master/spec/ruby/core}
LIMIT=${2:-150}

PASS=0
FAIL=0
ERR=0
CRASH=0
PROC=0
TIMEOUT=20

# Gather a sample of specs.  Take a wide mix across categories.
mapfile -t SPECS < <(find "$SPEC_BASE" -name "*_spec.rb" -type f | shuf | head -n "$LIMIT")

for f in "${SPECS[@]}"; do
    PROC=$((PROC + 1))
    out=$(timeout $TIMEOUT ./koruby_precise test/cruby_runner/run_rubyspec.rb "$f" 2>&1)
    code=$?
    # Look for the "pass=X fail=Y err=Z skip=W" trailer line.
    line=$(echo "$out" | grep -E "_spec.rb: pass=" | tail -1)
    if [ -z "$line" ]; then
        CRASH=$((CRASH + 1))
        continue
    fi
    p=$(echo "$line" | sed -E 's/.*pass=([0-9]+).*/\1/')
    fa=$(echo "$line" | sed -E 's/.*fail=([0-9]+).*/\1/')
    er=$(echo "$line" | sed -E 's/.*err=([0-9]+).*/\1/')
    PASS=$((PASS + p))
    FAIL=$((FAIL + fa))
    ERR=$((ERR + er))
done

echo "PASS=$PASS FAIL=$FAIL ERR=$ERR CRASH=$CRASH PROCESSED=$PROC"
