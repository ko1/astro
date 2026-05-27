#\!/usr/bin/env bash
# PGO+AOT bench.  Lines: id|k_cc|a_cc|k_pgo_flags|a_flags
set -u
KORUBY_DIR=/home/ko1/ruby/astro/sample/koruby
OPTCARROT="$KORUBY_DIR/../abruby/benchmark/optcarrot/bin/optcarrot-bench"
LOG="${1:-$KORUBY_DIR/tools/bench-results/pgo.log}"
FRAMES="${FRAMES:-300}"
RUNS="${RUNS:-5}"

cd "$KORUBY_DIR"

resolve_cc() {
  if [ -x "/usr/bin/$1" ]; then echo "/usr/bin/$1"; else echo "$1"; fi
}

run_pgo_config() {
  local id="$1" k_cc="$2" a_cc="$3" k_flags="$4" a_flags="$5"
  k_cc=$(resolve_cc "$k_cc")
  a_cc=$(resolve_cc "$a_cc")

  pkill -f "$KORUBY_DIR/koruby" 2>/dev/null || true
  sleep 0.3

  rm -f koruby
  rm -rf code_store pgo-data

  CC="$k_cc" optflags="$k_flags" make koruby-pgo 2>/tmp/_pgo.log >/dev/null
  if [ \! -s koruby ]; then
    echo "$id BUILD_FAIL_PGO  $(tail -1 /tmp/_pgo.log)" | tee -a "$LOG"
    return
  fi
  sync; sleep 0.2

  CC="$a_cc" CFLAGS="$a_flags -fPIC -fno-plt -fno-semantic-interposition" \
     LDFLAGS="-fno-lto" \
     ./koruby --aot-compile "$OPTCARROT" --frames 30 --headless \
       --no-print-fps --no-print-video-checksum >/tmp/_aot.log 2>&1
  if [ \! -e code_store/all.so ]; then
    echo "$id BUILD_FAIL_AOT" | tee -a "$LOG"
    return
  fi

  local size=$(stat -c %s code_store/all.so)
  taskset -c 0 ./koruby "$OPTCARROT" --frames 60 --headless \
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

  printf "%-50s size=%d best=%s median=%s mean=%s k_cc=%s a_cc=%s k=%q a=%q\n" \
    "$id" "$size" "$best" "$median" "$mean" "$k_cc" "$a_cc" "$k_flags" "$a_flags" \
    | tee -a "$LOG"
}

echo "==== PGO bench $(date -Iseconds) RUNS=$RUNS FRAMES=$FRAMES ====" | tee -a "$LOG"

while IFS='|' read -r id k_cc a_cc k_flags a_flags; do
  [ -z "$id" ] && continue
  case "$id" in \#*) continue ;; esac
  run_pgo_config "$id" "$k_cc" "$a_cc" "$k_flags" "$a_flags"
done
