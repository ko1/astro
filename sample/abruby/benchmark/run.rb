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

RUNNERS = [
  {
    name: 'ruby',
    cmd:  "ruby %s",
  },
  {
    name: 'ruby/jit',
    cmd:  'ruby --jit %s',
  },
  {
    name: 'abruby',
    cmd:  "ruby -I #{ABRUBY_DIR}/lib #{ABRUBY_DIR}/exe/abruby %s",
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
  cmd = runner[:cmd] % path
  env = runner[:env] || {}
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

def run_benchmark(runner, path, repeat)
  results = repeat.times.map { run_once(runner, path) }
  best = results.select { |r| r[:ok] }.min_by { |r| r[:time] }
  if best
    best
  else
    results.last  # return last failure for error reporting
  end
end

def format_time(t)
  if t < 0.01
    "%.1fms" % (t * 1000)
  elsif t < 1.0
    "%.0fms" % (t * 1000)
  else
    "%.3fs" % t
  end
end

def format_ratio(t, base)
  return "" if base.nil? || t.nil?
  ratio = t / base
  if ratio >= 1.0
    "%.1fx" % ratio
  else
    "%.2fx" % ratio
  end
end

# --- Main -----------------------------------------------------------------

repeat = 3
selected_runners = nil
list_mode = false

opt = OptionParser.new
opt.banner = "Usage: ruby benchmark/run.rb [options] [benchmark...]"
opt.on('-r RUNNERS', 'Comma-separated runner names') { |v| selected_runners = v.split(',') }
opt.on('-n N', Integer, 'Repeat count (take best)') { |v| repeat = v }
opt.on('-l', 'List available benchmarks') { list_mode = true }
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
col_width = [runners.map { |r| r[:name].size }.max + 2, 10].max

header = "%-#{name_width}s" % ""
runners.each { |r| header += "%#{col_width}s" % r[:name] }
# ratio columns (vs first runner)
if runners.size > 1
  runners[1..].each { |r| header += "%#{col_width}s" % "vs #{runners[0][:name]}" }
end
puts header
puts "-" * header.size

# Run benchmarks
benchmarks.each do |bm|
  row = "%-#{name_width}s" % bm[:name]
  times = []
  outputs = []

  runners.each do |runner|
    result = run_benchmark(runner, bm[:path], repeat)
    if result[:ok]
      row += "%#{col_width}s" % format_time(result[:time])
      times << result[:time]
      outputs << result[:output]
    else
      row += "%#{col_width}s" % "ERROR"
      times << nil
      outputs << nil
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

  if mismatch
    runners.each_with_index do |r, i|
      $stderr.puts "  #{r[:name]}: #{outputs[i].inspect}" if outputs[i]
    end
  end
end
