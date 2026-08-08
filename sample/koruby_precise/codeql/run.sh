#!/bin/sh
# CodeQL GC-borrow / encapsulation gate for koruby_precise.
# Run after changes:  make codeql-check
#
# Each rule is self-tested on a fixture (so the query can't silently regress to
# "always 0") and then required to be clean on the real koruby build trace.
# See codeql/README.md for what each rule enforces.  DBs live under codeql/.db
# (gitignored).  Needs the CodeQL CLI: gh extension install github/gh-codeql.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)

CQ=$(command -v codeql 2>/dev/null || true)
[ -z "$CQ" ] && CQ=$(ls -d "$HOME"/.local/share/gh/extensions/gh-codeql/dist/release/*/codeql 2>/dev/null | tail -1 || true)
[ -z "$CQ" ] && { echo "codeql CLI not found — install: gh extension install github/gh-codeql"; exit 2; }

DBDIR=${CODEQL_DBDIR:-$HERE/.db}
mkdir -p "$DBDIR"
( cd "$HERE" && "$CQ" pack install >/dev/null 2>&1 || true )

n_hits()   { grep -c "stale under moving GC" 2>/dev/null || true; }
n_viol()   { grep -c "outside an ARO_BORROW" 2>/dev/null || true; }
n_escape() { grep -c "escapes non-accessor" 2>/dev/null || true; }
n_unused() { grep -c "marked ARO_BORROW but" 2>/dev/null || true; }
runq()     { "$CQ" query run --database="$1" "$HERE/$2" 2>/dev/null; }

# interior-encapsulation ratchet: fail if direct interior accesses EXCEED this.
# 0 now that every payload access goes through an ARO_BORROW accessor.
ENCAP_BASELINE=0

# ---- fixture DBs (borrow_cases.c for temporal; annotation_cases.c for escape/unused) ----
FDB=$DBDIR/fixture
( cd "$HERE/test" && "$CQ" database create "$FDB" --language=cpp --overwrite \
    --command="gcc -c borrow_cases.c -o borrow_cases.o" ) >/dev/null 2>&1
rm -f "$HERE/test/borrow_cases.o"
ADB=$DBDIR/annot
( cd "$HERE/test" && "$CQ" database create "$ADB" --language=cpp --overwrite \
    --command="gcc -c annotation_cases.c -o annotation_cases.o" ) >/dev/null 2>&1
rm -f "$HERE/test/annotation_cases.o"

# ---- real koruby DB (build trace) ----
KDB=$DBDIR/koruby
"$CQ" database create "$KDB" --language=cpp --overwrite \
  --command="sh $HERE/cqbuild.sh" >/dev/null 2>&1

fail() { echo "FAIL: $1"; exit 1; }

# 1. borrow-after-gc  (temporal: a raw borrow held across a may-GC call)
[ "$(runq "$FDB" borrow_after_gc.ql | n_hits)" -eq 5 ] || fail "borrow_after_gc self-test != 5"
N=$(runq "$KDB" borrow_after_gc.ql | n_hits)
[ "$N" -eq 0 ] || { runq "$KDB" borrow_after_gc.ql; fail "$N borrow-after-gc hazard(s)"; }
echo "borrow-after-gc:        self-test 5 TP / 3 TN,  koruby 0 hazards        ok"

# 2. interior-encapsulation  (spatial: touching data_priv outside an accessor)
NV=$(runq "$KDB" interior_encapsulation.ql | n_viol)
[ "$NV" -le "$ENCAP_BASELINE" ] || fail "interior-encapsulation $NV > baseline $ENCAP_BASELINE (route via ARO_BORROW accessor)"
echo "interior-encapsulation: koruby $NV direct accesses (<= $ENCAP_BASELINE)     ok"

# 3. borrow-escape  (a raw borrow returned/stored by a non-ARO_BORROW function)
[ "$(runq "$ADB" borrow_escape.ql | n_escape)" -eq 1 ] || fail "borrow_escape self-test != 1"
NE=$(runq "$KDB" borrow_escape.ql | n_escape)
[ "$NE" -eq 0 ] || { runq "$KDB" borrow_escape.ql; fail "$NE borrow escape(s)"; }
echo "borrow-escape:          self-test 1,  koruby 0 escapes                  ok"

# 4. aro-borrow-unused  (ARO_BORROW function that touches no interior)
[ "$(runq "$ADB" aro_borrow_unused.ql | n_unused)" -eq 1 ] || fail "aro_borrow_unused self-test != 1"
NU=$(runq "$KDB" aro_borrow_unused.ql | n_unused)
[ "$NU" -eq 0 ] || { runq "$KDB" aro_borrow_unused.ql; fail "$NU unused ARO_BORROW annotation(s)"; }
echo "aro-borrow-unused:      self-test 1,  koruby 0 stale annotations        ok"
