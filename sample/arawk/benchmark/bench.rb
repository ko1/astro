#!/usr/bin/env ruby
# arawk benchmark harness — runs the goawk testdata/tt.* scripts on
# foo.td under {arawk-plain, arawk-aot, gawk, mawk, goawk} and reports
# the minimum runtime per test, normalised to gawk.
#
# Per-test scaling: each test is timed once on gawk and the input is
# repeated N times until that run takes ≥ MIN_TIME seconds.  Inputs
# are cached on disk to keep re-runs cheap.

require 'open3'
require 'fileutils'

ROOT     = File.expand_path('..', __dir__)
BIN      = File.join(ROOT, 'arawk')
GOAWKDIR = File.join(ROOT, 'goawk')
TESTDATA = File.join(GOAWKDIR, 'testdata')
INPUT    = File.join(TESTDATA, 'foo.td')
SCRATCH  = File.join(__dir__, 'scratch')
FileUtils.mkdir_p(SCRATCH)

GOAWK_BIN = File.join(ROOT, 'goawk-bin')

unless File.executable?(BIN)
  abort "arawk not built (run `make`)"
end
unless File.executable?(GOAWK_BIN)
  warn "warning: goawk-bin not built; building..."
  Dir.chdir(GOAWKDIR) do
    system('go', 'build', '-o', GOAWK_BIN, '.') || abort('go build failed')
  end
end

AWKS = [
  # label,             command + args (suffix: script_path, input_path appended at run-time)
  ['arawk-plain',      [BIN, '--plain', '-f']],
  ['arawk-aot',        [BIN, '-f']],
  ['gawk',             ['gawk', '-f']],
  ['mawk',             ['mawk', '-f']],
  ['goawk',            [GOAWK_BIN, '-f']],
]

# Tests where arawk doesn't implement the features (matches the skip
# table in test/run_bench_tests.rb).  We still bench the others so the
# table shows arawk's coverage progress.
SKIP_FOR_ARAWK = %w[
  tt.08z_regex_simple
  tt.09_regex_starts_with
  tt.10_regex_ends_with
  tt.10a_regex_ends_with_var
  tt.15_format_lines
  tt.big_complex_program
]

MIN_TIME = 1.0   # seconds — the memory rule says ≥ 1 s per cell
RUNS     = 5

def scaled_input(test_path)
  # Probe gawk timing once; repeat foo.td N times to reach MIN_TIME.
  probe = Time.now
  system('gawk', '-f', test_path, INPUT, out: File::NULL, err: File::NULL)
  elapsed = Time.now - probe
  return INPUT if elapsed >= MIN_TIME

  mult = (MIN_TIME / [elapsed, 0.001].max).ceil
  cache = File.join(SCRATCH, "foo.td.x#{mult}")
  unless File.exist?(cache) && File.size(cache) == File.size(INPUT) * mult
    File.open(cache, 'wb') do |out|
      content = File.binread(INPUT)
      mult.times { out.write(content) }
    end
  end
  cache
end

def run_once(cmd, script, input)
  full = cmd + [script, input]
  start = Time.now
  Open3.capture3(*full, stdin_data: '')   # ignore stdout
  Time.now - start
end

def fmt(t)
  '%7.3f' % t
end

# AOT prebake: clear code_store, do one `-c` run per script so the
# arawk-aot column doesn't pay the bake cost in the timed runs.
def prebake(script, input)
  FileUtils.rm_rf(File.join(ROOT, 'code_store'))
  system(BIN, '-c', '--ccs', '-f', script, input, out: File::NULL, err: File::NULL)
end

results = {}
tests = Dir.glob("#{TESTDATA}/tt.*").sort

puts format("%-30s | %s", 'Test', AWKS.map { |l, _| '%10s' % l }.join(' | '))
puts '-' * 30 + ('-+-' + '-' * 10) * AWKS.size

tests.each do |script|
  name = File.basename(script)
  arawk_unsupported = SKIP_FOR_ARAWK.include?(name)
  input = scaled_input(script)
  prebake(script, input) unless arawk_unsupported

  row = AWKS.map do |label, cmd|
    if arawk_unsupported && label.start_with?('arawk')
      next 'n/a'
    end
    times = []
    RUNS.times do
      t = run_once(cmd, script, input)
      times << t
    end
    times.min
  end
  results[name] = row

  cells = row.map { |v| v.is_a?(Float) ? fmt(v) : '%10s' % v }
  puts format("%-30s | %s", name, cells.join(' | '))
end

# Normalised geomean vs gawk (excluding tests where any required column is n/a).
gawk_idx = AWKS.index { |l, _| l == 'gawk' }
puts
puts 'Normalised speedup vs gawk (lower = slower; >1 = faster):'
puts format("%-30s | %s", 'Test', AWKS.map { |l, _| '%10s' % l }.join(' | '))
puts '-' * 30 + ('-+-' + '-' * 10) * AWKS.size

product = Array.new(AWKS.size, 1.0)
count = 0
results.each do |name, row|
  gawk_t = row[gawk_idx]
  next unless gawk_t.is_a?(Float)
  ratios = row.map { |v| v.is_a?(Float) ? gawk_t / v : v }
  ratios.each_with_index do |r, i|
    product[i] *= r if r.is_a?(Float)
  end
  count += 1
  cells = ratios.map { |v| v.is_a?(Float) ? '%10.2f' % v : '%10s' % v }
  puts format("%-30s | %s", name, cells.join(' | '))
end

if count > 0
  cells = product.map { |p| '%10.2f' % (p ** (1.0 / count)) }
  puts '-' * 30 + ('-+-' + '-' * 10) * AWKS.size
  puts format("%-30s | %s", 'geomean', cells.join(' | '))
end
