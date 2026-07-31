#!/bin/sh
# rubyboy_setup.sh — fetch the pure-Ruby Game Boy emulator (sacckey/rubyboy) for
# the rubyboy benchmark.  Sources are NOT committed (see .gitignore); this clones
# them into rubyharness/apps/rubyboy on demand.  Idempotent: re-running is a no-op.
# The test ROM (lib/roms/tobu.gb) ships inside the repo, so no extra download.
#
#   sh tools/rubyboy_setup.sh
#
# Env:
#   RUBYBOY_REPO  git URL           (default sacckey/rubyboy)
#   RUBYBOY_REF   branch/tag/commit (default: the rev this harness was validated against)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../apps/rubyboy"
RUBYBOY_REPO=${RUBYBOY_REPO:-https://github.com/sacckey/rubyboy.git}
RUBYBOY_REF=${RUBYBOY_REF:-a60a944720ec11e3aebd819e352aa779b1008e48}

mkdir -p "$HERE/../apps"

if [ ! -d "$APP/.git" ]; then
  echo "cloning $RUBYBOY_REPO -> apps/rubyboy"
  git clone "$RUBYBOY_REPO" "$APP"
  ( cd "$APP" && git checkout "$RUBYBOY_REF" )
else
  echo "apps/rubyboy already present (skip clone)"
fi

ROM="$APP/lib/roms/tobu.gb"
[ -f "$ROM" ] || { echo "ERROR: test ROM missing: $ROM"; exit 1; }
echo "rubyboy benchmark ready: $APP (ROM $(wc -c < "$ROM") bytes)"
