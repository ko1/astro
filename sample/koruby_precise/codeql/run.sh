#!/bin/sh
# CodeQL GC-borrow gate for koruby_precise.  Run after changes:  make codeql-check
#
# Two phases, both must pass:
#   1. self-test  — build a DB from test/borrow_cases.c and require the query to
#                   find EXACTLY the 2 injected bugs (guards the query itself
#                   from silently regressing to "always 0").
#   2. real-code  — build a DB from the koruby build trace and require
#                   borrow_after_gc.ql to report 0 hazards.
#
# Needs the CodeQL CLI (gh extension install github/gh-codeql) + `codeql pack
# install` (auto-run below).  DBs go under codeql/.db (gitignored).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)

# Locate the CodeQL CLI: PATH, then the gh-codeql extension's unpacked release.
CQ=$(command -v codeql 2>/dev/null || true)
[ -z "$CQ" ] && CQ=$(ls -d "$HOME"/.local/share/gh/extensions/gh-codeql/dist/release/*/codeql 2>/dev/null | tail -1 || true)
[ -z "$CQ" ] && { echo "codeql CLI not found — install: gh extension install github/gh-codeql"; exit 2; }

DBDIR=${CODEQL_DBDIR:-$HERE/.db}
mkdir -p "$DBDIR"
( cd "$HERE" && "$CQ" pack install >/dev/null 2>&1 || true )

count_hits() { grep -c "stale under moving GC" 2>/dev/null || true; }
count_viol() { grep -c "outside an ARO_BORROW" 2>/dev/null || true; }

# interior-encapsulation ratchet: fail if direct interior accesses EXCEED this.
# Lower it as accesses are routed through ARO_BORROW inline accessors.
ENCAP_BASELINE=0

# --- 1. query self-test on the fixture (expect exactly 2) ---
FDB=$DBDIR/fixture
( cd "$HERE/test" && "$CQ" database create "$FDB" --language=cpp --overwrite \
    --command="gcc -c borrow_cases.c -o borrow_cases.o" ) >/dev/null 2>&1
rm -f "$HERE/test/borrow_cases.o"
NF=$("$CQ" query run --database="$FDB" "$HERE/borrow_after_gc.ql" 2>/dev/null | count_hits)
if [ "$NF" -ne 5 ]; then
  echo "FAIL: self-test expected 5 fixture bugs, got $NF — the query regressed."
  exit 1
fi
echo "self-test: query catches the 5 fixture bugs (5 TP / 3 TN) ok"

# --- 2. real-code check (expect 0) ---
KDB=$DBDIR/koruby
"$CQ" database create "$KDB" --language=cpp --overwrite \
  --command="sh $HERE/cqbuild.sh" >/dev/null 2>&1
OUT=$("$CQ" query run --database="$KDB" "$HERE/borrow_after_gc.ql" 2>/dev/null)
N=$(printf '%s\n' "$OUT" | count_hits)
if [ "$N" -ne 0 ]; then
  echo "FAIL: $N borrow-after-gc hazard(s) in koruby_precise:"
  printf '%s\n' "$OUT"
  exit 1
fi
echo "koruby_precise: borrow-after-gc clean (0 hazards) ok"

# --- 3. interior-encapsulation ratchet (no NEW direct interior accesses) ---
NV=$("$CQ" query run --database="$KDB" "$HERE/interior_encapsulation.ql" 2>/dev/null | count_viol)
if [ "$NV" -gt "$ENCAP_BASELINE" ]; then
  echo "FAIL: interior-encapsulation regressed: $NV > baseline $ENCAP_BASELINE."
  echo "      Route new KorbStrBuf::data / KorbArrayItems::data access through an"
  echo "      ARO_BORROW-marked inline accessor (or lower ENCAP_BASELINE if you removed some)."
  exit 1
fi
echo "interior-encapsulation: $NV direct accesses (<= baseline $ENCAP_BASELINE) ok"
