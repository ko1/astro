#!/usr/bin/env ruby
# frozen_string_literal: true
#
# abruby benchmark runner
#
# Usage:
#   ruby benchmark/run.rb                  # run all benchmarks
#   ruby benchmark/run.rb fib tak          # run specific benchmarks
#   ruby benchmark/run.rb -r ruby,abruby   # select runners
#   ruby benchmark/run.rb -n 3             # repeat N times, take best
#   ruby benchmark/run.rb -l               # list available benchmarks
#
# Runners are defined in RUNNERS below.  Add new entries to compare
# ruby --yjit, abruby with different optimization flags, etc.

require 'optparse'
require 'open3'

ABRUBY_DIR = File.expand_path('..', __dir__)
BENCHMARK_DIR = __dir__

# --- Runner definitions ---------------------------------------------------
#
# Each runner has:
#   name:    short label for the column header
#   cmd:     command template.  "%s" is replaced with the benchmark file path.
#   env:     optional environment variables hash
#
# Add new runners here as needed (e.g. ruby --yjit, abruby --opt, etc.)

STORE = "#{ABRUBY_DIR}/code_store"

# Each runner:
#   name:  column label
#   cmd:   timed command; "%s" is the benchmark path
#   setup: optional, runs ONCE before any iteration (e.g. populate the
#          store for steady-state runners)
#   prep:  optional, runs before EVERY iteration (e.g. rm -rf store for
#          cold-start runners so best-of-N truly measures cold runs)
#   env:   optional environment variables hash
#
# Steady-state rows (abruby+aot / abruby+pgc) prepare the code store in
# `setup` (not counted) and measure pure execution in `cmd`.  This is
# apples-to-apples with ruby --jit (which is also measured steady-state).
# Cold-start rows (abruby+cf) use `prep` to reset between iterations.
RUNNERS = [
  {
    name: 'ruby',
    cmd:  "ruby %s",
  },
  {
    name: 'ruby+jit',
    cmd:  'ruby --jit %s',
  },
  {
    name: 'abruby+plain',
    cmd:  "ruby -I #{ABRUBY_DIR}/lib #{ABRUBY_DIR}/exe/abruby --plain %s",
  },
  {
    # AOT cold-start: compile + run in one process.  Shows the one-shot
    # "from scratch" cost (time includes all SD_<Horg>.c + all.so build).
    # `prep` clears the store before *every* iteration so best-of-N
    # actually measures cold runs — if rm were in setup only, 2nd/3rd
    # iterations would skip compilation and best would collapse to
    # steady-state.  prep is not timed.
    name:  'abruby+cf',
    prep:  "rm -rf #{STORE}",
    cmd:   "CCACHE_DISABLE=1 ruby -I #{ABRUBY_DIR}/lib #{ABRUBY_DIR}/exe/abruby -c %s",  # -c = --aot-compile-first
  },
  {
    # AOT steady-state: prime the store in setup, time a clean
    # --compiled-only --aot-only run.
    name:  'abruby+aot',
    setup: "rm -rf #{STORE} && CCACHE_DISABLE=1 ruby -I #{ABRUBY_DIR}/lib #{ABRUBY_DIR}/exe/abruby --aot-compile %s",
    cmd:   "ruby -I #{ABRUBY_DIR}/lib #{ABRUBY_DIR}/exe/abruby --compiled-only --aot-only %s",
  },
  {
    # Full PGC bake: threshold=0 so every parsed entry gets baked (PGSD_
    # if profile differentiates it, SD_<Horg> otherwise).  Steady-state
    # run follows — no interpreter fallback.  This is the historical
    # "max PGC" baseline; use for apples-to-apples comparisons against
    # pre-threshold runs.
    name:  'abruby+pgc-all',
    setup: "rm -rf #{STORE} && CCACHE_DISABLE=1 ruby -I #{ABRUBY_DIR}/lib #{ABRUBY_DIR}/exe/abruby --pg-compile --pg-threshold=0 %s",
    cmd:   "ruby -I #{ABRUBY_DIR}/lib #{ABRUBY_DIR}/exe/abruby --compiled-only %s",
  },
  {
    # Threshold-gated PG-compile: dispatch_count >= 100 → PGSD/SD baked,
    # the rest (cold) runs as plain interpreter.  Faster bake, smaller
    # all.so; steady-state cost of cold entries is what the runtime
    # number captures.  Realistic default for iterative development.
    name:  'abruby+pgc',
    setup: "rm -rf #{STORE} && CCACHE_DISABLE=1 ruby -I #{ABRUBY_DIR}/lib #{ABRUBY_DIR}/exe/abruby --pg-compile %s",
    cmd:   "ruby -I #{ABRUBY_DIR}/lib #{ABRUBY_DIR}/exe/abruby --compiled-only %s",
  },
]

# --- Helpers --------------------------------------------------------------

def discover_benchmarks
  Dir.glob(File.join(BENCHMARK_DIR, 'bm_*.ab.rb')).sort.map do |path|
    name = File.basename(path, '.ab.rb').sub(/\Abm_/, '')
    { name: name, path: path }
  end
end

def run_once(runner, path)
  env = runner[:env] || {}
  # Per-iteration prep (e.g. rm -rf code_store for cold-start cf runs).
  # Runs before the clock starts so it doesn't land in the timing.
  if runner[:prep]
    system(env, runner[:prep] % path, out: File::NULL, err: File::NULL)
  end
  cmd = runner[:cmd] % path
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  out, err, status = Open3.capture3(env, cmd)
  t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  {
    time:   t1 - t0,
    output: out.strip,
    error:  err.strip,
    ok:     status.success?,
  }
end

def run_setup(runner, path)
  return true unless runner[:setup]
  cmd = runner[:setup] % path
  out, err, status = Open3.capture3(runner[:env] || {}, cmd)
  unless status.success?
    $stderr.puts "setup failed for #{runner[:name]}: #{cmd}"
    $stderr.puts err unless err.empty?
    return false
  end
  true
end

def run_benchmark(runner, path, repeat)
  # Setup runs once per (runner, benchmark) — not counted in timing.
  return { ok: false, time: nil, output: "", error: "setup failed", all_times: [] } \
    unless run_setup(runner, path)
  results = repeat.times.map { run_once(runner, path) }
  best = results.select { |r| r[:ok] }.min_by { |r| r[:time] }
  best ||= results.last  # return last failure for error reporting
  best[:all_times] = results.select { |r| r[:ok] }.map { |r| r[:time] }
  best
end

def format_time(t)
  "%.3fs" % t
end

def format_ratio(t, base)
  return "" if base.nil? || t.nil?
  ratio = t / base
  "%.2fx" % ratio
end

# --- Main -----------------------------------------------------------------

repeat = 3
selected_runners = nil
list_mode = false
show_all = false

opt = OptionParser.new
opt.banner = "Usage: ruby benchmark/run.rb [options] [benchmark...]"
opt.on('-r RUNNERS', 'Comma-separated runner names') { |v| selected_runners = v.split(',') }
opt.on('-n N', Integer, 'Repeat count (take best)') { |v| repeat = v }
opt.on('-l', 'List available benchmarks') { list_mode = true }
opt.on('-a', '--all', 'Show all iteration times') { show_all = true }
opt.parse!(ARGV)

benchmarks = discover_benchmarks

if list_mode
  benchmarks.each { |bm| puts bm[:name] }
  exit
end

# Filter benchmarks by name if specified
unless ARGV.empty?
  names = ARGV.map(&:downcase)
  benchmarks = benchmarks.select { |bm| names.include?(bm[:name].downcase) }
end

# Filter runners if specified
runners = if selected_runners
  RUNNERS.select { |r| selected_runners.include?(r[:name]) }
else
  RUNNERS
end

if benchmarks.empty?
  $stderr.puts "No benchmarks found."
  exit 1
end

if runners.empty?
  $stderr.puts "No runners selected."
  exit 1
end

# Header
name_width = [benchmarks.map { |bm| bm[:name].size }.max, 12].max
ratio_labels = runners.size > 1 ? runners[1..].map { |r| "#{r[:name]}/#{runners[0][:name]}" } : []
all_labels = runners.map { |r| r[:name] } + ratio_labels
col_width = [all_labels.map { |l| l.size }.max + 2, 10].max

header = "%-#{name_width}s" % ""
runners.each { |r| header += "%#{col_width}s" % r[:name] }
ratio_labels.each { |l| header += "%#{col_width}s" % l }
puts header
puts "-" * header.size

# Run benchmarks
benchmarks.each do |bm|
  row = "%-#{name_width}s" % bm[:name]
  times = []
  outputs = []

  all_detail = []

  runners.each do |runner|
    result = run_benchmark(runner, bm[:path], repeat)
    if result[:ok]
      row += "%#{col_width}s" % format_time(result[:time])
      times << result[:time]
      outputs << result[:output]
      all_detail << [runner[:name], result[:all_times]]
    else
      row += "%#{col_width}s" % "ERROR"
      times << nil
      outputs << nil
      all_detail << [runner[:name], []]
    end
  end

  # Ratio columns
  if runners.size > 1
    base_time = times[0]
    times[1..].each do |t|
      if base_time && t
        row += "%#{col_width}s" % format_ratio(t, base_time)
      else
        row += "%#{col_width}s" % "-"
      end
    end
  end

  # Verify outputs match
  valid_outputs = outputs.compact.uniq
  mismatch = valid_outputs.size > 1
  row += "  *** MISMATCH ***" if mismatch

  puts row

  if show_all && repeat > 1
    all_detail.each do |name, atimes|
      next if atimes.empty?
      puts "  %-#{name_width}s %s" % [name, atimes.map { |t| format_time(t) }.join(", ")]
    end
  end

  if mismatch
    runners.each_with_index do |r, i|
      $stderr.puts "  #{r[:name]}: #{outputs[i].inspect}" if outputs[i]
    end
  end
end
