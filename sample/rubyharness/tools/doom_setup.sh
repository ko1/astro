#!/bin/sh
# doom_setup.sh — fetch the pure-Ruby DOOM app + shareware WAD for the DOOM
# benchmark.  The sources and WAD are NOT committed (see .gitignore); this clones
# them into rubyharness/apps/doom on demand.  Idempotent: re-running is a no-op.
#
#   sh tools/doom_setup.sh
#
# Env:
#   DOOM_REPO   git URL           (default khasinski/doom)
#   DOOM_REF    branch/tag/commit (default: default branch)
#   WAD_URL     shareware WAD URL (default Akbar30Bill/DOOM_wads doom1.wad)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../apps/doom"
DOOM_REPO=${DOOM_REPO:-https://github.com/khasinski/doom.git}
WAD_URL=${WAD_URL:-https://raw.githubusercontent.com/Akbar30Bill/DOOM_wads/master/doom1.wad}

mkdir -p "$HERE/../apps"

if [ ! -d "$APP/.git" ]; then
  echo "cloning $DOOM_REPO -> apps/doom"
  git clone --depth 1 "$DOOM_REPO" "$APP"
  if [ -n "$DOOM_REF" ]; then ( cd "$APP" && git fetch --depth 1 origin "$DOOM_REF" && git checkout "$DOOM_REF" ); fi
else
  echo "apps/doom already present (skip clone)"
fi

WAD="$APP/doom1.wad"
if [ ! -f "$WAD" ]; then
  echo "downloading shareware WAD -> apps/doom/doom1.wad"
  curl -fsSL -o "$WAD" "$WAD_URL"
fi

# Sanity: valid IWAD (magic "IWAD" or "PWAD").
magic=$(dd if="$WAD" bs=1 count=4 2>/dev/null)
case "$magic" in
  IWAD|PWAD) echo "WAD ok ($magic, $(wc -c < "$WAD") bytes)";;
  *) echo "ERROR: $WAD is not a valid WAD (magic=$magic)"; exit 1;;
esac
echo "doom benchmark ready: $APP"
