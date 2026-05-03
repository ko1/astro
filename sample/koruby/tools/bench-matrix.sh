#!/usr/bin/env bash
# Systematic compiler/flags exploration for koruby+optcarrot.
# Usage: tools/bench-matrix.sh [results_file]   (configs from stdin)
#
# Each config (line: id|k_cc|a_cc|k_flags|a_flags):
#   1. Build koruby (interp) with $k_cc + $k_flags.  koruby's own
#      Makefile applies LTO; only all.so is forbidden from LTO per
#      project rule.
#   2. Bake AOT directly via ./koruby --aot-compile (bypassing the
#      koruby-aot make wrapper so we control CC/CFLAGS/LDFLAGS
#      cleanly).  CFLAGS MUST NOT contain -flto.  LDFLAGS forces
#      -fno-lto on the all.so link step.
#   3. Warm up once at WARMUP_FRAMES.  Then run optcarrot RUNS times
#      at FRAMES headless, taskset -c 0.  Log best/median/mean.
# No `set -e`: we want to keep iterating even if one config fails.

KORUBY_DIR=/home/ko1/ruby/astro/sample/koruby
OPTCARROT="$KORUBY_DIR/../abruby/benchmark/optcarrot/bin/optcarrot-bench"
LOG="${1:-$KORUBY_DIR/tools/bench-matrix.log}"
FRAMES="${FRAMES:-300}"
RUNS="${RUNS:-5}"
WARMUP_FRAMES="${WARMUP_FRAMES:-60}"

cd "$KORUBY_DIR"

# Resolve to /usr/bin/<cc> if it exists, else passthrough — bypasses
# /usr/lib/ccache/ wrappers that fail under sandboxed home.
resolve_cc() {
  if [ -x "/usr/bin/$1" ]; then
    echo "/usr/bin/$1"
  else
    echo "$1"
  fi
}

run_config() {
  local id="$1" k_cc="$2" a_cc="$3" k_flags="$4" a_flags="$5"
  k_cc=$(resolve_cc "$k_cc")
  a_cc=$(resolve_cc "$a_cc")

  # Kill any leftover koruby/cc/make subprocesses that may still hold
  # the binary or be writing to code_store — otherwise the rm + make
  # sequence races ("Text file busy", "directory not empty").
  pkill -9 -f "$KORUBY_DIR/koruby" 2>/dev/null || true
  pkill -9 -f "code_store/o" 2>/dev/null || true
  pkill -9 cc1 2>/dev/null || true
  sleep 0.5

  rm -f koruby
  # Retry rm of code_store — concurrent parallel make may have fresh
  # files we need to wait out.
  local r=0
  while [ -e code_store ] && [ $r -lt 5 ]; do
    rm -rf code_store 2>/dev/null
    [ -e code_store ] || break
    sleep 0.5
    r=$((r+1))
  done
  rm -rf code_store 2>/dev/null  # final attempt, ignore errors

  # clang's LTO bitcode needs lld; system ld may produce empty output
  # silently.  Add -fuse-ld=lld-21 when k_cc is a clang.
  local k_extra=""
  case "$k_cc" in
    *clang*) k_extra="-fuse-ld=lld-21" ;;
  esac

  CC="$k_cc" optflags="$k_flags $k_extra" make 2>/tmp/_kbuild.log >/dev/null
  if [ ! -x koruby ] || [ ! -s koruby ]; then
    local err=$(tail -1 /tmp/_kbuild.log 2>/dev/null)
    echo "$id BUILD_FAIL_KORUBY  $err" | tee -a "$LOG"
    return
  fi
  sync
  sleep 0.2

  # Retry exec a few times if "Text file busy" — it's transient.
  local aot_tries=0
  while [ $aot_tries -lt 3 ]; do
    CC="$a_cc" CFLAGS="$a_flags -fPIC -fno-plt -fno-semantic-interposition" \
       LDFLAGS="-fno-lto" \
       ./koruby --aot-compile "$OPTCARROT" --frames 30 --headless \
         --no-print-fps --no-print-video-checksum >/tmp/_aot.log 2>&1
    if [ -e code_store/all.so ]; then break; fi
    aot_tries=$((aot_tries+1))
    sleep 1
  done
  if [ ! -e code_store/all.so ]; then
    local err=$(tail -1 /tmp/_aot.log 2>/dev/null)
    echo "$id BUILD_FAIL_AOT  $err" | tee -a "$LOG"
    return
  fi

  local size=$(stat -c %s code_store/all.so)

  taskset -c 0 ./koruby "$OPTCARROT" --frames "$WARMUP_FRAMES" --headless \
    --no-print-fps --no-print-video-checksum >/dev/null 2>&1 || true

  local vals=()
  for i in $(seq 1 $RUNS); do
    local fps=$(taskset -c 0 ./koruby "$OPTCARROT" --frames "$FRAMES" \
      --headless --no-print-video-checksum 2>&1 | grep "^fps:" | awk '{print $2}')
    if [ -n "$fps" ]; then
      vals+=("$fps")
    fi
  done

  if [ ${#vals[@]} -eq 0 ]; then
    echo "$id NO_FPS" | tee -a "$LOG"
    return
  fi

  local sorted=$(printf "%s\n" "${vals[@]}" | sort -n)
  local best=$(echo "$sorted" | tail -1)
  local n=${#vals[@]}
  local mid=$(( (n+1) / 2 ))
  local median=$(echo "$sorted" | awk -v m=$mid 'NR==m { print; exit }')
  local mean=$(echo "$sorted" | awk '{s+=$1; n++} END { printf "%.3f", s/n }')

  printf "%-50s size=%d best=%s median=%s mean=%s k_cc=%s a_cc=%s k=%q a=%q\n" \
    "$id" "$size" "$best" "$median" "$mean" "$k_cc" "$a_cc" "$k_flags" "$a_flags" \
    | tee -a "$LOG"
}

echo "==== bench-matrix run $(date -Iseconds) RUNS=$RUNS FRAMES=$FRAMES ====" | tee -a "$LOG"
echo "rule: no -flto in a_flags; LDFLAGS=-fno-lto for all.so" | tee -a "$LOG"

while IFS='|' read -r id k_cc a_cc k_flags a_flags; do
  if [ -z "$id" ]; then
    continue
  fi
  case "$id" in
    \#*) continue ;;
  esac
  run_config "$id" "$k_cc" "$a_cc" "$k_flags" "$a_flags"
done
