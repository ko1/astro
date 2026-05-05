#!/usr/bin/env ruby
#
# Multi-engine jq benchmark.
#
# Engines compared:
#   - jq           — reference implementation (system /usr/bin/jq)
#   - jaq          — Rust-implemented jq, ~30× faster startup
#   - gojq         — pure-Go reimplementation (jq-1.7 compat)
#   - nuq interp   — this project, no Code Store specialisation
#   - nuq AOT      — this project, with Code Store baked SDs
#
# Filters are borrowed from jaq's `examples/benches/` (the de-facto
# bench suite for the jq language family).  Each filter takes a single
# integer `n` over stdin and returns a `length`-style scalar so output
# is small.
#
# Usage:
#   ruby bench/bench.rb              # all benches, default n
#   ruby bench/bench.rb fib          # only the fib bench
#   ruby bench/bench.rb fib n=10000  # override n for fib
#
# CCACHE_DISABLE=1 is set automatically (see project memory for the
# ccache + sandbox flake).
#
# Per-cell timeout is 30 s; cells that exceed are reported as `timeout`.

require 'open3'
require 'fileutils'
require 'timeout'

ROOT = File.expand_path('..', __dir__)
NUQ  = File.join(ROOT, 'nuq')
abort "nuq binary missing — run `make` first" unless File.executable?(NUQ)

ENGINES = []
ENGINES << ['jq',       ['/usr/bin/jq', '-c']]                     if File.executable?('/usr/bin/jq')
ENGINES << ['jaq',      ['/tmp/claude/bin/jaq', '-c']]             if File.executable?('/tmp/claude/bin/jaq')
ENGINES << ['gojq',     ['/tmp/claude/bin/gojq', '-c']]            if File.executable?('/tmp/claude/bin/gojq')
ENGINES << ['nuq int',  [NUQ, '-c', '--no-compile']]
ENGINES << ['nuq AOT',  [NUQ, '-c'], { aot: true }]

# n values are tuned so jq runs in roughly 0.1s..2s.  jaq's defaults
# go up to 1M; that's tractable for jaq (compiled Rust) but stretches
# nuq's tree-walker.  We use jaq's defaults where nuq can keep up,
# and lower them where O(n²) operations (add, reduce) blow up.
DEFAULT_N = {
  'empty'       =>      1,
  'upto'        =>   8192,
  'reverse'     => 1_000_000,
  'sort'        =>  300_000,
  'add'         =>     2_000,   # O(n²) array-of-array concat: low n
  'kv'          =>     5_000,   # O(n²) object collision lookup: low n
  'min-max'     => 1_000_000,
  'last'        => 1_000_000,
  'try-catch'   =>  500_000,
  'cumsum'      =>  500_000,
  'group-by'    =>  100_000,
  'pyramid'     =>   8_000,    # recursive multi-emit; nuq is O(n²)
  'to-fromjson' =>  100_000,
  'ack'         =>      7,
}

PER_CELL_TIMEOUT = 30   # seconds
ATTEMPTS = 3

def run_with_timeout(cmd, env, stdin_data, timeout: PER_CELL_TIMEOUT)
  # Use Open3.popen3 so we have the PID for timeout-based kill.
  $stderr.puts "  RUN: #{cmd.inspect}  env=#{env.inspect}  stdin=#{stdin_data.inspect}" if ENV['BENCH_DEBUG']
  killed = false
  Open3.popen3(env, *cmd) do |stdin, stdout, stderr, wait_thr|
    pid = wait_thr.pid
    stdin.write(stdin_data)
    stdin.close
    out_io = Thread.new { stdout.read }
    err_io = Thread.new { stderr.read }
    if wait_thr.join(timeout).nil?
      Process.kill('KILL', pid) rescue nil
      wait_thr.value rescue nil
      killed = true
    end
    out_io.join; err_io.join
    return [out_io.value, err_io.value, wait_thr.value, killed]
  end
end

def measure(cmd, env, stdin_data, attempts: ATTEMPTS, timeout: PER_CELL_TIMEOUT)
  best = nil
  attempts.times do
    t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    out, err, st, killed = run_with_timeout(cmd, env, stdin_data, timeout: timeout)
    t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    if killed
      return [nil, "timeout (#{timeout}s)", false]
    end
    return [t1 - t0, err.lines.first&.chomp, false] unless st && st.success?
    elapsed = t1 - t0
    best = elapsed if best.nil? || elapsed < best
  end
  [best, nil, true]
end

def fmt(t)
  return '   —    ' if t.nil?
  if t < 0.01
    sprintf('%4.1f ms ', t * 1000)
  elsif t < 1.0
    sprintf('%4.0f ms ', t * 1000)
  else
    sprintf('%4.2f s  ', t)
  end
end

# ---- arg parsing ------------------------------------------------------

selected_names = []
override_n = nil
ARGV.each do |a|
  if a =~ /\An=(\d+)\z/
    override_n = $1.to_i
  else
    selected_names << a
  end
end

names = Dir["#{__dir__}/filters/*.jq"].map { |p| File.basename(p, '.jq') }.sort
names.select! { |n| selected_names.include?(n) } unless selected_names.empty?

# ---- main loop --------------------------------------------------------

env_aot     = { 'CCACHE_DISABLE' => '1' }
env_neutral = {}

# Header
header_engine_cells = ENGINES.map { |label, _| label.center(8) }
puts ''
printf "%-13s %8s   %s\n", 'bench', 'n', header_engine_cells.join('   ')
puts '-' * (13 + 8 + 3 + ENGINES.length * 11)

results = {}

names.each do |name|
  n = override_n || DEFAULT_N[name] || 1
  filter_path = File.join(__dir__, 'filters', "#{name}.jq")
  filter = File.read(filter_path).strip
  stdin_data = "#{n}\n"

  row = ENGINES.map do |label, cmd, opts|
    opts ||= {}
    if opts[:aot]
      # Wipe code_store before AOT so we measure cached run after first bake.
      FileUtils.rm_rf(File.join(ROOT, 'code_store'))
      # Bake (1st run, throw-away timing).
      _bake_t, _bake_err, _bake_ok = measure(cmd + [filter], env_aot, stdin_data, attempts: 1)
      # Cached runs (best of 3).
      t, err, ok = measure(cmd + [filter], env_aot, stdin_data)
    else
      t, err, ok = measure(cmd + [filter], env_neutral, stdin_data)
    end
    [t, err, ok]
  end

  results[name] = row
  cells = row.map { |t, _, _| fmt(t) }
  printf "%-13s %8d   %s\n", name, n, cells.join('   ')

  # Print first-line errors to stderr so we don't lose them
  row.each_with_index do |(t, err, ok), i|
    if !ok && err
      $stderr.puts "  [#{ENGINES[i][0]}] #{err}"
    end
  end
end

# Print speedup summary relative to jq
puts ''
puts "Speedup vs jq (lower is faster; <1.0 means faster than jq):"
puts ''
header = "%-13s   %s\n" % ['bench', ENGINES.map { |label, _| label.center(8) }.join('   ')]
print header
puts '-' * (13 + 3 + ENGINES.length * 11)

jq_idx = ENGINES.index { |label, _| label == 'jq' }
results.each do |name, row|
  ref = jq_idx ? row[jq_idx][0] : nil
  cells = row.map do |t, _, _|
    if t.nil? || ref.nil? || ref == 0
      '   —    '
    else
      r = t / ref
      if r < 1.0
        sprintf('%5.2fx⬇ ', 1.0 / r)
      else
        sprintf('%5.2fx⬆ ', r)
      end
    end
  end
  printf "%-13s   %s\n", name, cells.join('   ')
end

puts ''
puts "Versions:"
ENGINES.each do |label, cmd, _|
  v = `#{cmd[0]} --version 2>/dev/null`.lines.first&.chomp || '?'
  puts "  #{label}: #{cmd[0]}  (#{v})"
end
puts "  CCACHE_DISABLE=1, per-cell timeout #{PER_CELL_TIMEOUT}s, best-of-#{ATTEMPTS}"
