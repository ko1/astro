#!/bin/sh
# Run one rubyspec file through the real mspec and show the failures.
# Usage: tools/one.sh <rel-spec-path> [extra mspec args...]
K="$(cd "$(dirname "$0")/.." && pwd)"
f="$1"; shift
mkdir -p "${TMPDIR:-/tmp}/koruby_one"
cd "$HOME/ruby/src/master/spec/ruby" || exit 1
# RUBY_FLAGS: mspec 本体は spawn 時に必ず立てる。ここは launcher 経由で
# その経路を通らないので自分で空に立てる (無いと fixture の
# ENV["RUBY_FLAGS"].split で落ちる。CRuby でも同じに落ちる)。
MSPEC_RUNNER=1 RUBY_FLAGS="" SPEC_TEMP_DIR="${TMPDIR:-/tmp}/koruby_one" \
  timeout 90 "$K/koruby_precise" "$K/tools/mspec_launch.rb" "$@" "$f" 2>&1 |
  grep -v "mspec_launch.rb:"
