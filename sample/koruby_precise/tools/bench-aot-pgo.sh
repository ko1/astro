#!/usr/bin/env bash
# AOT-level PGO bench: profile-guided rebuild of code_store/all.so.
#
# 3-pass per config:
#   1. Build koruby (normal, with k_flags).
#   2. Build AOT with -fprofile-generate=$PGO_DIR.
#   3. Run optcarrot N profile frames.
#   4. Rebuild code_store/o/*.o with -fprofile-use=$PGO_DIR (same source,
#      different -c options).  Re-link all.so.
#   5. Bench at FRAMES x RUNS.
#
# Lines: id|k_cc|a_cc|k_flags|a_flags
# k_flags is for koruby's `optflags` (Makefile sees `optflags="$k_flags"`).
# a_flags is the AOT base CFLAGS (e.g. "-O2 -march=native").
# AOT step always adds -fPIC -fno-plt -fno-semantic-interposition.
KORUBY_DIR=/home/ko1/ruby/astro/sample/koruby
OPTCARROT="$KORUBY_DIR/../abruby/benchmark/optcarrot/bin/optcarrot-bench"
LOG="${1:-$KORUBY_DIR/tools/bench-results/aot_pgo.log}"
FRAMES="${FRAMES:-300}"
RUNS="${RUNS:-10}"
PGO_FRAMES="${PGO_FRAMES:-60}"
WARMUP_FRAMES="${WARMUP_FRAMES:-60}"

cd "$KORUBY_DIR"

resolve_cc() {
  if [ -x "/usr/bin/$1" ]; then echo "/usr/bin/$1"; else echo "$1"; fi
}

run_aot_pgo_config() {
  local id="$1" k_cc="$2" a_cc="$3" k_flags="$4" a_flags="$5"
  k_cc=$(resolve_cc "$k_cc")
  a_cc=$(resolve_cc "$a_cc")

  pkill -9 -f "$KORUBY_DIR/koruby" 2>/dev/null || true
  pkill -9 cc1 2>/dev/null || true
  sleep 0.5

  local PGO_DIR="$KORUBY_DIR/aot-pgo-data"
  rm -f koruby
  rm -rf code_store "$PGO_DIR"
  mkdir -p "$PGO_DIR"

  # KORUBY_PGO=1 → use Makefile's koruby-pgo target (instrument →
  # optcarrot 60f profile → re-link).  Otherwise plain `make`.
  if [ -n "${KORUBY_PGO:-}" ]; then
    rm -rf pgo-data
    CC="$k_cc" optflags="$k_flags" make koruby-pgo 2>/tmp/_kbuild.log >/dev/null
  else
    CC="$k_cc" optflags="$k_flags" make 2>/tmp/_kbuild.log >/dev/null
  fi
  if [ ! -s koruby ]; then
    echo "$id BUILD_FAIL_KORUBY  $(tail -1 /tmp/_kbuild.log)" | tee -a "$LOG"
    return
  fi
  sync; sleep 0.2

  # Pass 1: AOT with -fprofile-generate.  -fprofile-generate must be on
  # both compile (CFLAGS) and link (LDFLAGS) so libgcov is linked in.
  local cflags_inst="$a_flags -fPIC -fno-plt -fno-semantic-interposition -fprofile-generate=$PGO_DIR"
  CC="$a_cc" CFLAGS="$cflags_inst" \
     LDFLAGS="-fno-lto -fprofile-generate=$PGO_DIR" \
     ./koruby --aot-compile "$OPTCARROT" --frames 30 --headless \
       --no-print-fps --no-print-video-checksum >/tmp/_aot1.log 2>&1
  if [ ! -e code_store/all.so ]; then
    echo "$id BUILD_FAIL_AOT_INST  $(tail -1 /tmp/_aot1.log)" | tee -a "$LOG"
    return
  fi

  # Run profile-collection workload.  No taskset — just collect.
  ./koruby "$OPTCARROT" --frames "$PGO_FRAMES" --headless \
    --no-print-fps --no-print-video-checksum >/dev/null 2>&1
  # Verify .gcda files exist
  local n_gcda=$(find "$PGO_DIR" -name '*.gcda' | wc -l)
  if [ "$n_gcda" -eq 0 ]; then
    echo "$id NO_GCDA" | tee -a "$LOG"
    return
  fi

  # Pass 2: rebuild code_store/o/* with -fprofile-use.  Need to rm .o
  # files (Makefile rule rebuilds), keep .c files.  Then re-make.
  rm -f code_store/o/*.o code_store/all.so
  local cflags_use="$a_flags -fPIC -fno-plt -fno-semantic-interposition -fprofile-use=$PGO_DIR -fprofile-correction"
  CC="$a_cc" CFLAGS="$cflags_use" LDFLAGS="-fno-lto" \
     make -C code_store -s all.so 2>/tmp/_aot2.log
  if [ ! -e code_store/all.so ]; then
    echo "$id BUILD_FAIL_AOT_USE  $(tail -1 /tmp/_aot2.log)" | tee -a "$LOG"
    return
  fi

  local size=$(stat -c %s code_store/all.so)

  taskset -c 0 ./koruby "$OPTCARROT" --frames "$WARMUP_FRAMES" --headless \
    --no-print-fps --no-print-video-checksum >/dev/null 2>&1 || true

  local vals=()
  for i in $(seq 1 $RUNS); do
    local fps=$(taskset -c 0 ./koruby "$OPTCARROT" --frames "$FRAMES" \
      --headless --no-print-video-checksum 2>&1 | grep "^fps:" | awk '{print $2}')
    [ -n "$fps" ] && vals+=("$fps")
  done

  if [ ${#vals[@]} -eq 0 ]; then
    echo "$id NO_FPS" | tee -a "$LOG"; return
  fi

  local sorted=$(printf "%s\n" "${vals[@]}" | sort -n)
  local best=$(echo "$sorted" | tail -1)
  local n=${#vals[@]}
  local mid=$(( (n+1) / 2 ))
  local median=$(echo "$sorted" | awk -v m=$mid 'NR==m { print; exit }')
  local mean=$(echo "$sorted" | awk '{s+=$1; n++} END { printf "%.3f", s/n }')

  printf "%-50s size=%d best=%s median=%s mean=%s gcda=%d k_cc=%s a_cc=%s k=%q a=%q\n" \
    "$id" "$size" "$best" "$median" "$mean" "$n_gcda" "$k_cc" "$a_cc" "$k_flags" "$a_flags" \
    | tee -a "$LOG"
}

echo "==== AOT-PGO bench $(date -Iseconds) RUNS=$RUNS FRAMES=$FRAMES PGO_FRAMES=$PGO_FRAMES ====" | tee -a "$LOG"
echo "rule: -fno-lto on all.so link; -fprofile-{generate,use}= on AOT compile" | tee -a "$LOG"

while IFS='|' read -r id k_cc a_cc k_flags a_flags; do
  [ -z "$id" ] && continue
  case "$id" in \#*) continue ;; esac
  run_aot_pgo_config "$id" "$k_cc" "$a_cc" "$k_flags" "$a_flags"
done
