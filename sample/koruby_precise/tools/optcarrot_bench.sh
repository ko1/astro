#!/usr/bin/env bash
# optcarrot across the same modes as the normal bench (cruby / cruby+yjit /
# interp / aot+cached), reporting BOTH wall time (s, best of RUNS) and the
# emulator's own fps.  Usage: tools/optcarrot_bench.sh [FRAMES] [RUNS]
#
# CRuby modes run the real optcarrot (bin/optcarrot); the koruby modes run the
# require-free bundle (--plain for interp, bake + --compiled-only for aot).  All
# print "fps:"/"checksum:"; checksums are cross-checked so a wrong-but-fast cell
# can't masquerade as a win.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
OPT="$HERE/../abruby/benchmark/optcarrot"
FRAMES=${1:-180}
RUNS=${2:-2}
BIN="$HERE/koruby_precise"
RUBY=${RUBY:-ruby}
NES=examples/Lan_Master.nes

BUNDLE=$(OPTC_MODE=build "$HERE/tools/optcarrot.sh" "$FRAMES")
cd "$OPT" || exit 1

# run_mode LABEL CMD...  → prints "LABEL<TAB>best_time<TAB>fps<TAB>checksum"
run_mode() {
  local label=$1; shift
  local best= fps= cks=
  local i t0 t1 out dt f k
  for i in $(seq 1 "$RUNS"); do
    t0=$(date +%s.%N)
    out=$("$@" 2>/dev/null)
    t1=$(date +%s.%N)
    dt=$(awk "BEGIN{printf \"%.3f\", $t1-$t0}")
    f=$(printf '%s\n' "$out" | awk -F': ' '/^fps/{print $2}')
    k=$(printf '%s\n' "$out" | awk -F': ' '/^checksum/{print $2}')
    if [ -z "$best" ] || awk "BEGIN{exit ($dt < $best)?0:1}"; then best=$dt; fps=$f; fi
    cks=$k
  done
  printf '%s\t%s\t%s\t%s\n' "$label" "${best:-NA}" "${fps:-NA}" "${cks:-NA}"
}

TMP=$(mktemp)
run_mode cruby       $RUBY --yjit-disable -Ilib bin/optcarrot --benchmark --frames "$FRAMES" "$NES" >>"$TMP"
run_mode cruby+yjit  $RUBY --yjit         -Ilib bin/optcarrot --benchmark --frames "$FRAMES" "$NES" >>"$TMP"
run_mode interp      "$BIN" --plain "$BUNDLE"            >>"$TMP"

# AOT: time the cold bake once, then the warm --compiled-only run (best of RUNS).
# aot+compile = bake + warm run (cold start, includes the C compile);
# aot+cached  = warm run only (store reused) — same as bench-report's split.
bt0=$(date +%s.%N)
rm -rf code_store
CCACHE_DISABLE=1 "$BIN" --aot-compile "$BUNDLE" >/dev/null 2>&1
bt1=$(date +%s.%N)
bake=$(awk "BEGIN{printf \"%.3f\", $bt1-$bt0}")
warm=$(run_mode aot+cached "$BIN" --compiled-only "$BUNDLE")   # "aot+cached\tt\tfps\tcks"
wt=$(printf '%s' "$warm" | cut -f2); wf=$(printf '%s' "$warm" | cut -f3); wk=$(printf '%s' "$warm" | cut -f4)
printf 'aot+compile\t%s\t%s\t%s\n' "$(awk "BEGIN{printf \"%.3f\", $bake+$wt}")" "$wf" "$wk" >>"$TMP"
printf '%s\n' "$warm" >>"$TMP"

# fps ratio baseline = YJIT (the strongest competitor).
fbase=$(awk -F'\t' '$1=="cruby+yjit"{print $3}' "$TMP")
printf '%-12s %10s %10s %10s\n' bench time_s fps fps/yjit
while IFS=$'\t' read -r label t f k; do
  fr=$(awk "BEGIN{printf \"%.2f\", $f/$fbase}")
  f2=$(awk "BEGIN{printf \"%.1f\", $f}")
  printf '%-12s %10s %10s %10s\n' "$label" "$t" "$f2" "$fr"
done < "$TMP"

# checksum sanity: every mode must agree (else the fps/time are meaningless).
ncks=$(awk -F'\t' '{print $4}' "$TMP" | sort -u | wc -l)
allk=$(awk -F'\t' '{print $4}' "$TMP" | sort -u | paste -sd,)
if [ "$ncks" -eq 1 ]; then echo "checksum: $allk (all modes agree)"; else echo "CHECKSUM MISMATCH: $allk"; fi
rm -f "$TMP"
