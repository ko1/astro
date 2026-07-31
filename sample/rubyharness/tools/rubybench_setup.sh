#!/bin/sh
# rubybench_setup.sh — fetch ruby/ruby-bench (the yjit-bench suite) for the
# rubybench harness.  Sources are NOT committed (see .gitignore); this clones
# them into rubyharness/apps/ruby-bench on demand.  Idempotent.
#
#   sh tools/rubybench_setup.sh
#
# Env: RB_REPO (default ruby/ruby-bench), RB_REF (branch/tag/commit).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../apps/ruby-bench"
RB_REPO=${RB_REPO:-https://github.com/ruby/ruby-bench.git}

mkdir -p "$HERE/../apps"
if [ ! -d "$APP/.git" ]; then
  echo "cloning $RB_REPO -> apps/ruby-bench"
  git clone --depth 1 "$RB_REPO" "$APP"
  [ -n "$RB_REF" ] && ( cd "$APP" && git fetch --depth 1 origin "$RB_REF" && git checkout "$RB_REF" )
else
  echo "apps/ruby-bench already present (skip clone)"
fi
[ -d "$APP/benchmarks" ] || { echo "ERROR: $APP/benchmarks missing"; exit 1; }
echo "ruby-bench ready: $APP ($(ls "$APP"/benchmarks/*.rb | wc -l) single-file micros)"
