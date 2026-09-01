#!/bin/sh
# Run one rubyspec file through the real mspec and show the failures.
# Usage: tools/one.sh <rel-spec-path> [extra mspec args...]
K="$(cd "$(dirname "$0")/.." && pwd)"
f="$1"; shift
mkdir -p "${TMPDIR:-/tmp}/koruby_one"
cd "$HOME/ruby/src/master/spec/ruby" || exit 1
MSPEC_RUNNER=1 SPEC_TEMP_DIR="${TMPDIR:-/tmp}/koruby_one" \
  timeout 90 "$K/koruby_precise" "$K/tools/mspec_launch.rb" "$@" "$f" 2>&1 |
  grep -v "mspec_launch.rb:"
