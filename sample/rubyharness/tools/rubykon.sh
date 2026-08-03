#!/bin/sh
# rubykon.sh — pure-Ruby Go (Baduk) Monte-Carlo AI benchmark (PragTob/rubykon,
# shipped inside ruby/ruby-bench).  Runs a seeded batch of MCTS searches on a
# fixed board (multi-file `require`, NOT bundled — exercises the require-AOT
# path) and prints an FNV checksum of the chosen best moves.  Deterministic:
# CRuby and an ASTro sample must agree.  Wall clock is timed by the caller.
#
# Env:
#   RB_MODE   plain (default) — tree-walker (INTERP --plain)
#             aot             — --aot-compile --run (discover+bake), then --compiled-only
#             cruby           — $RUBY --yjit-disable (oracle)
#             cruby-yjit      — $RUBY --yjit
#             build           — write the driver, print its path, exit
#   INTERP    sample binary   (default ./koruby_precise)
#   RUBY      reference ruby  (default ruby)
#   GAMES / ITERS / SIZE / SEED   workload knobs (default 20 / 100 / 9 / 42)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../apps/ruby-bench/benchmarks/rubykon"
MODE=${RB_MODE:-plain}
INTERP=${INTERP:-./koruby_precise}
RUBY=${RUBY:-ruby}

[ -d "$APP/lib" ] || sh "$HERE/rubybench_setup.sh"
[ -d "$APP/lib" ] || { echo "rubykon not found under ruby-bench (apps/ruby-bench/benchmarks/rubykon)"; exit 2; }

case "$INTERP" in /*|*" "*) ;; ./*) INTERP="$(cd "$(dirname "$INTERP")" && pwd)/$(basename "$INTERP")";; esac

# Deterministic headless driver (seeded MCTS → best-move checksum).  The default
# run_benchmark harness p's the whole search tree (megabytes, with non-reproducible
# object ids), so we extract just the stable best move instead.
DRV="$APP/rubykon_headless.rb"
cat > "$DRV" <<'RB'
$LOAD_PATH.unshift(File.expand_path('lib', __dir__))
require 'rubykon'

srand((ENV['SEED'] || 42).to_i)
size  = (ENV['SIZE']  || 9).to_i
iters = (ENV['ITERS'] || 100).to_i
games = (ENV['GAMES'] || 20).to_i

h = 14695981039346656037
games.times do
  gs   = Rubykon::GameState.new(Rubykon::Game.new(size))
  mcts = MCTS::MCTS.new
  root = mcts.start(gs, iters)
  s = root.best_move.to_s
  s.each_byte { |b| h = ((h ^ b) * 1099511628211) & 0xffffffffffffffff }
end
puts "games=#{games} size=#{size} iters=#{iters} checksum=#{h}"
RB

case "$MODE" in
  build)      echo "$DRV"; exit 0 ;;
  cruby)      exec "$RUBY" --yjit-disable "$DRV" ;;
  cruby-yjit) exec "$RUBY" --yjit "$DRV" ;;
  aot)
    cd "$APP"; rm -rf code_store
    CCACHE_DISABLE=1 "$INTERP" --aot-compile --run "$DRV" >/dev/null 2>&1 || { echo "aot-compile failed"; exit 1; }
    exec "$INTERP" --compiled-only "$DRV" ;;
  plain|*)    exec "$INTERP" --plain "$DRV" ;;
esac
