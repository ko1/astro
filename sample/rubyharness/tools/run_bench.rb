#!/usr/bin/env ruby
# frozen_string_literal: true
#
# run_bench.rb — multi-mode micro-benchmark driver for the koruby rebuild.
#
# Runs each bench/*.rb under every requested EXECUTION MODE and reports a table
# of best-of-N wall times (min = least noise) + a geomean slowdown vs CRuby.
#
# Modes:
#   cruby         ruby --yjit-disable
#   cruby+yjit    ruby --yjit
#   interp        interpreter, code_store wiped (pure tree-walk)
#   aot+compile   cold AOT: TIME INCLUDES the --aot-compile build + one run
#   aot+cached    warm AOT: build once (untimed), TIME the cached run only.
#                 Run with ASTRO_AOT_STRICT=1 — a strict interpreter must exit
#                 non-zero if it ever falls back to interp dispatch, so the cell
#                 becomes INTERP! when the AOT is not actually pure.  (koruby does
#                 not honour the flag yet → its number can hide silent fallback.)
#   pg+cached     warm PG (--pg-compile); only for samples that implement PG.
#
# Each mode's stdout is compared to CRuby's (MISMATCH guards wrong-but-fast).
# Benches are ~1s on CRuby so interpreter startup is negligible.  Runs on CRuby.
#
# Usage: ruby run_bench.rb --interp KORUBY [--ref ruby] [--dir bench]
#          [--runs N] [--timeout SEC] [--pattern '*.rb'] [--modes a,b,c]
require 'open3'
require 'fileutils'

opts = { ref: 'ruby', dir: 'bench', runs: 5, timeout: 300, pattern: '*.rb', modes: nil }
args = ARGV.dup
until args.empty?
  case (a = args.shift)
  when '--interp'  then opts[:interp]  = args.shift
  when '--ref'     then opts[:ref]     = args.shift
  when '--dir'     then opts[:dir]     = args.shift
  when '--runs'    then opts[:runs]    = Integer(args.shift)
  when '--timeout' then opts[:timeout] = Integer(args.shift)
  when '--pattern' then opts[:pattern] = args.shift
  when '--modes'   then opts[:modes]   = args.shift
  else abort "unknown option: #{a}"
  end
end
abort 'need --interp KORUBY' unless opts[:interp]

koruby = opts[:interp].split   # may be wrapped: "env ASTRO_GC_STRESS=1 ./koruby_precise"
ruby   = opts[:ref].split
WIPE   = -> { FileUtils.rm_rf('code_store') }
compile = ->(flag, f) { system(*koruby, flag, f, out: File::NULL, err: File::NULL) }

# Each mode: prep (untimed), timed_setup (timed, added once), run (best-of-N),
# env (extra env for the run), strict (use ASTRO_AOT_STRICT for the no-interp check).
MODES = {
  'cruby'       => { run: ->(f) { ruby + ['--yjit-disable', f] } },
  'cruby+yjit'  => { run: ->(f) { ruby + ['--yjit', f] } },
  'interp'      => { prep: ->(_f) { WIPE.call }, run: ->(f) { koruby + [f] } },
  'aot+compile' => { prep: ->(_f) { WIPE.call }, timed_setup: ->(f) { compile.call('--aot-compile', f) },
                     run: ->(f) { koruby + [f] } },
  'aot+cached'  => { prep: ->(f) { WIPE.call; compile.call('--aot-compile', f) },
                     run: ->(f) { koruby + [f] }, env: { 'ASTRO_AOT_STRICT' => '1' }, strict: true },
  'pg+cached'   => { prep: ->(f) { WIPE.call; compile.call('--pg-compile', f) },
                     run: ->(f) { koruby + [f] }, env: { 'ASTRO_AOT_STRICT' => '1' }, strict: true },
}.freeze
DEFAULT = %w[cruby cruby+yjit interp aot+compile aot+cached].freeze
modes = (opts[:modes]&.split(',') || DEFAULT)
modes.each { |m| MODES.key?(m) || abort("unknown mode: #{m} (have: #{MODES.keys.join(', ')})") }

def best(env, cmd, runs, secs)
  best_t = out = nil
  code = 0
  runs.times do
    t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    o, _e, st = Open3.capture3(env, 'timeout', '-k', '2', secs.to_s, *cmd)
    t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    code = st.exited? ? st.exitstatus : (st.signaled? ? 128 + st.termsig : 1)
    break if code != 0
    out = o.strip
    dt = t1 - t0
    best_t = dt if best_t.nil? || dt < best_t
  end
  [best_t, out, code]
end

def measure(mode, f, runs, secs)
  mode[:prep]&.call(f)
  extra = 0.0
  if mode[:timed_setup]
    t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    mode[:timed_setup].call(f)
    extra = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
  end
  t, out, code = best(mode[:env] || {}, mode[:run].call(f), runs, secs)
  [t && t + extra, out, code]
end

W = [11, *modes.map(&:size)].max
benches = Dir.glob(File.join(opts[:dir], opts[:pattern])).sort
abort "no benches under #{opts[:dir]}" if benches.empty?

geo = Hash.new { |h, k| h[k] = [] }
puts(format("%-10s #{(['%%%ds'] * modes.size).join('  ')}", 'bench', *Array.new(modes.size, W)) % modes)
benches.each do |f|
  ref_out = nil
  cells = modes.map { |mn| [mn, *measure(MODES[mn], f, opts[:runs], opts[:timeout])] }
  ref_out = cells.find { |c| c[0] == 'cruby' }&.dig(2)
  base    = cells.find { |c| c[0] == 'cruby' }&.dig(1) || cells.map { |c| c[1] }.compact.min
  printed = cells.map do |mn, t, out, code|
    cell = if code == 124 then 'TIMEOUT'
           elsif code && code >= 128 then "CRASH#{code - 128}"
           elsif MODES[mn][:strict] && code == 7 then 'INTERP!' # strict: fell back to interp
           elsif ref_out && out != ref_out then 'MISMATCH'
           else
             geo[mn] << (t / base) if t && base && base > 0
             format('%.3f', t)
           end
    format("%#{W}s", cell)
  end
  puts(format('%-10s %s', File.basename(f, '.rb'), printed.join('  ')))
end
gline = modes.map do |mn|
  rs = geo[mn]
  format("%#{W}s", rs.empty? ? '-' : format('%.2fx', Math.exp(rs.sum { |r| Math.log(r) } / rs.size)))
end
puts(format('%-10s %s', 'geomean', gline.join('  ')))
