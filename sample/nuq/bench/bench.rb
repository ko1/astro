#!/usr/bin/env ruby
#
# nuq vs jq/jaq/gojq benchmark.
#
# Two suites:
#   1. real/   — realistic jq workloads on a synthetic JSON dataset
#                (bench/data/users.json, ~1.9 MB / 10k user objects).
#                Filters are file paths or stdin pipes — input is a
#                full JSON file.
#   2. micro/  — micro-benchmarks borrowed from jaq's examples/benches.
#                Input is just an integer `n` over stdin.
#
# Each cell measures **whole-process wall time** via Open3.popen3 +
# Process.wait, including:
#   - shell spawn / exec
#   - filter parser
#   - JSON input parse
#   - filter evaluation
#   - output write
#   - process exit
#
# This matches what users care about for command-line jq usage.
#
# Usage:
#   ruby bench/bench.rb              # both suites
#   ruby bench/bench.rb real         # real suite only
#   ruby bench/bench.rb micro        # micro suite only
#   ruby bench/bench.rb fib          # filter benches by name (substring)
#   BENCH_DEBUG=1 ruby bench/bench.rb # show cmd lines
#
# Env:
#   JQ=path/to/jq      override jq binary
#   JAQ=...            override jaq binary
#   GOJQ=...           override gojq binary

require 'open3'
require 'fileutils'

ROOT = File.expand_path('..', __dir__)
NUQ  = File.join(ROOT, 'nuq')
abort "nuq binary missing — run `make` first" unless File.executable?(NUQ)

JQ_BIN   = ENV['JQ']   || '/usr/bin/jq'
JAQ_BIN  = ENV['JAQ']  || '/tmp/claude/bin/jaq'
GOJQ_BIN = ENV['GOJQ'] || '/tmp/claude/bin/gojq'

ENGINES = []
ENGINES << ['jq',      [JQ_BIN, '-c']]                     if File.executable?(JQ_BIN)
ENGINES << ['jaq',     [JAQ_BIN, '-c']]                    if File.executable?(JAQ_BIN)
ENGINES << ['gojq',    [GOJQ_BIN, '-c']]                   if File.executable?(GOJQ_BIN)
ENGINES << ['nuq int', [NUQ, '-c', '--no-compile']]
ENGINES << ['nuq AOT', [NUQ, '-c'], { aot: true }]

PER_CELL_TIMEOUT = 30
ATTEMPTS = 3

# n values for micro benches (stdin scalar)
MICRO_N = {
  'empty'       =>      1,
  'upto'        =>   8192,
  'reverse'     => 1_000_000,
  'sort'        =>  300_000,
  'add'         =>     2_000,
  'kv'          =>     5_000,
  'min-max'     => 1_000_000,
  'last'        => 1_000_000,
  'try-catch'   =>  500_000,
  'cumsum'      =>  500_000,
  'group-by'    =>  100_000,
  'pyramid'     =>   8_000,
  'to-fromjson' =>  100_000,
  'ack'         =>      7,
}

# real benches: all use bench/data/users.json by default
REAL_INPUT = File.join(__dir__, 'data', 'users.json')

def run_with_timeout(cmd, env, stdin_data: nil, stdin_file: nil, timeout: PER_CELL_TIMEOUT)
  $stderr.puts "  RUN: #{cmd.inspect}  env=#{env.inspect}  stdin=#{stdin_data ? stdin_data.inspect : "<#{stdin_file}>"}" if ENV['BENCH_DEBUG']
  killed = false
  Open3.popen3(env, *cmd) do |stdin, stdout, stderr, wait_thr|
    pid = wait_thr.pid
    begin
      if stdin_file
        File.open(stdin_file, 'rb') { |f| IO.copy_stream(f, stdin) }
      elsif stdin_data
        stdin.write(stdin_data)
      end
    rescue Errno::EPIPE
      # child closed stdin before reading all of it (e.g. `.` only
      # parses one value, then exits) — that's fine.
    end
    stdin.close rescue nil
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

def measure(cmd, env, stdin_data: nil, stdin_file: nil, attempts: ATTEMPTS)
  best = nil
  attempts.times do
    t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    out, err, st, killed = run_with_timeout(cmd, env, stdin_data: stdin_data, stdin_file: stdin_file)
    t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    if killed
      return [nil, "timeout (#{PER_CELL_TIMEOUT}s)", false]
    end
    return [t1 - t0, err.lines.first&.chomp, false] unless st && st.success?
    elapsed = t1 - t0
    best = elapsed if best.nil? || elapsed < best
  end
  [best, nil, true]
end

def fmt_time(t)
  return '—' if t.nil?
  if t < 0.01
    sprintf('%.1f ms', t * 1000)
  elsif t < 1.0
    sprintf('%.0f ms', t * 1000)
  else
    sprintf('%.2f s ', t)
  end
end

def fmt_speedup(jq_t, t)
  return '—' if jq_t.nil? || t.nil? || t == 0
  r = jq_t / t   # > 1 means t is faster than jq
  sprintf('%.2fx', r)
end

# ---- arg parsing ------------------------------------------------------

selected = []
suites = []
ARGV.each do |a|
  if %w[real micro].include?(a)
    suites << a
  else
    selected << a
  end
end
suites = %w[real micro] if suites.empty?

# ---- bench loop -------------------------------------------------------

env_aot     = { 'CCACHE_DISABLE' => '1' }
env_neutral = {}

def run_suite(suite, names, default_n, stdin_provider, env_aot, env_neutral)
  results = []
  names.each do |name|
    filter_path = File.join(__dir__, suite, "#{name}.jq")
    next unless File.exist?(filter_path)
    filter = File.read(filter_path).strip
    stdin_data, stdin_file = stdin_provider.call(name)

    row = [name]
    row_data = []
    ENGINES.each do |label, cmd, opts|
      opts ||= {}
      if opts[:aot]
        FileUtils.rm_rf(File.join(ROOT, 'code_store'))
        # bake (1 attempt, throw-away timing)
        measure(cmd + [filter], env_aot, stdin_data: stdin_data, stdin_file: stdin_file, attempts: 1)
        # measure (best of 3 cached runs)
        t, err, ok = measure(cmd + [filter], env_aot, stdin_data: stdin_data, stdin_file: stdin_file)
      else
        t, err, ok = measure(cmd + [filter], env_neutral, stdin_data: stdin_data, stdin_file: stdin_file)
      end
      row_data << [t, err, ok]
    end
    results << [name, row_data]
  end
  results
end

# Real suite: filters apply to bench/data/users.json
real_names = Dir["#{__dir__}/real/*.jq"].map { |p| File.basename(p, '.jq') }.sort
real_names.select! { |n| selected.any? { |s| n.include?(s) } } unless selected.empty?
real_results = []
if suites.include?('real')
  real_results = run_suite('real', real_names,
                           nil,
                           ->(_name) { [nil, REAL_INPUT] },
                           env_aot, env_neutral)
end

# Micro suite: stdin = "n\n" (scalar)
micro_names = Dir["#{__dir__}/filters/*.jq"].map { |p| File.basename(p, '.jq') }.sort
micro_names.select! { |n| selected.any? { |s| n.include?(s) } } unless selected.empty?
micro_results = []
if suites.include?('micro')
  micro_results = run_suite('filters', micro_names,
                            MICRO_N,
                            ->(name) { ["#{MICRO_N[name] || 1}\n", nil] },
                            env_aot, env_neutral)
end

# ---- MD table output --------------------------------------------------

def print_md_table(title, results, n_provider)
  return if results.empty?
  puts ''
  puts "## #{title}"
  puts ''
  header = ['bench']
  header << 'n' if n_provider
  header += ENGINES.map { |label, _| label }
  puts '| ' + header.join(' | ') + ' |'
  puts '|' + (['---'] * header.length).join('|') + '|'
  results.each do |name, row|
    cells = [name]
    cells << n_provider.call(name).to_s if n_provider
    row.each do |t, _, ok|
      cells << (ok ? fmt_time(t) : (t.nil? ? '—' : "**err**"))
    end
    puts '| ' + cells.join(' | ') + ' |'
  end

  jq_idx = ENGINES.index { |label, _| label == 'jq' }
  return unless jq_idx

  # Speedup table relative to jq
  puts ''
  puts "### Speedup vs jq (higher = faster than jq)"
  puts ''
  header = ['bench']
  header << 'n' if n_provider
  header += ENGINES.map { |label, _| label }
  puts '| ' + header.join(' | ') + ' |'
  puts '|' + (['---'] * header.length).join('|') + '|'
  results.each do |name, row|
    jq_t = row[jq_idx][0]
    cells = [name]
    cells << n_provider.call(name).to_s if n_provider
    row.each do |t, _, ok|
      cells << (ok ? fmt_speedup(jq_t, t) : '—')
    end
    puts '| ' + cells.join(' | ') + ' |'
  end
end

puts "# nuq benchmark results"
print_md_table("Real-world (input: bench/data/users.json, ~1.9 MB / 10k users)",
               real_results, nil)
print_md_table("Micro-benchmarks (jaq examples/benches; input = scalar n via stdin)",
               micro_results, ->(name) { MICRO_N[name] })

puts ''
puts "## Versions"
puts ''
ENGINES.each do |label, cmd, _|
  v = `#{cmd[0]} --version 2>/dev/null`.lines.first&.chomp || '?'
  puts "- **#{label}**: `#{cmd[0]}` (#{v})"
end
puts "- per-cell timeout: #{PER_CELL_TIMEOUT}s, best-of-#{ATTEMPTS}, CCACHE_DISABLE=1 for AOT cells"
puts "- whole-process wall time (includes startup, filter parse, JSON parse, eval, output)"
