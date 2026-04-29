#!/usr/bin/env ruby
# Compare castro across optimization tiers vs gcc on a few C programs.
#
# Tiers (label terminology mirrors sample/abruby/benchmark/run.rb):
#   interp     — pure interpreter, no code-store          (run only)
#   AOT first  — clear cache + ASTro-specialize + run     (compile included)
#   AOT cached — code_store/all.so on disk, dlopen + run  (run only)
#   gcc -O0..-O3 — pre-built; run only (compile separate)
#
# Each measurement is the median of N runs (default 5).
#
# Usage: ruby bench.rb              # full table
#        ruby bench.rb -h           # help
#        BENCH_RUNS=3 ruby bench.rb # fewer iterations

require 'fileutils'
require 'open3'

HERE   = __dir__
CASTRO = File.expand_path('castro', HERE)
BIN    = File.expand_path('tmp/refbin', HERE)
RUNS   = (ENV['BENCH_RUNS'] || 5).to_i

# Redirect ccache to a writable dir; never silently re-use cached gcc
# compiles from a previous run when measuring "1st" timings.
ENV['CCACHE_DIR'] = File.expand_path('tmp/ccache', HERE)
FileUtils.mkdir_p(ENV['CCACHE_DIR'])

abort "castro not built — run 'make' first." unless File.executable?(CASTRO)

# Inputs are small enough that interp finishes in <30s.  Pick sizes so
# even -O3 takes >50ms (so timing noise doesn't dominate).
BENCHES = [
  'fib_big.c',          # int recursion
  'fib_d.c',            # double recursion
  'tak.c',              # nested calls
  'ackermann.c',        # deep recursion
  'loop_sum.c',         # tight integer loop
  'mandelbrot_count.c', # double loop, no I/O
].freeze

def time_ms(*cmd, env: {})
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  out, err, st = Open3.capture3(env, *cmd)
  t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  unless st.success? || st.exitstatus
    warn "fail: #{cmd.inspect} #{err}"
  end
  ((t1 - t0) * 1000).to_i
end

def median(xs)
  s = xs.sort
  s[s.size / 2]
end

def measure(runs:, fresh: false, &block)
  median(runs.times.map { FileUtils.rm_rf('code_store') if fresh; block.call })
end

# Build gcc reference binaries up front
def build_refs(c_file, levels)
  base = File.basename(c_file, '.c')
  levels.each do |lvl|
    out = File.join(BIN, "#{base}_#{lvl}")
    cmd = ['gcc', "-O#{lvl}", '-o', out, File.join(HERE, 'examples', c_file)]
    sys, err, st = Open3.capture3(*cmd)
    unless st.success?
      warn "ref build failed: #{cmd.inspect}"
      warn err
      exit 1
    end
  end
end

def fmt_ms(ms)  = '%6d' % ms
def fmt_ratio(r) = (r ? '%5.2fx' % r : '   --')

LEVELS = %w[0 1 2 3]

FileUtils.mkdir_p(BIN)

# Build reference binaries for each
puts "building gcc reference binaries..."
BENCHES.each { |c| build_refs(c, LEVELS) }

header_cols = ['bench', 'interp', 'AOT first', 'AOT cached', 'gcc-O0', 'gcc-O1', 'gcc-O2', 'gcc-O3', 'spd-vO3']
puts '%-20s %s' % [header_cols[0], header_cols[1..].map { |c| '%10s' % c }.join(' ')]
puts '-' * 102

BENCHES.each do |c_file|
  base = File.basename(c_file, '.c')
  c_path = File.join(HERE, 'examples', c_file)

  # interp
  interp_ms = measure(runs: RUNS) { time_ms(CASTRO, '-q', '--no-compile', c_path) }

  # castro: 1st (cold, compile + run)
  c1_ms = measure(runs: RUNS, fresh: true) { time_ms(CASTRO, '-q', '--compile-all', c_path) }

  # castro: hot cache (cache prebuilt, just dlopen+run)
  ch_ms = measure(runs: RUNS) { time_ms(CASTRO, '-q', c_path) }

  # gcc -O0..-O3
  gcc_ms = LEVELS.map do |l|
    bin = File.join(BIN, "#{base}_#{l}")
    measure(runs: RUNS) { time_ms(bin) }
  end
  o3 = gcc_ms.last
  speedup_vs_o3 = o3.to_f.positive? ? ch_ms.to_f / o3 : nil

  cells = [
    fmt_ms(interp_ms),
    fmt_ms(c1_ms),
    fmt_ms(ch_ms),
    *gcc_ms.map { |m| fmt_ms(m) },
    fmt_ratio(speedup_vs_o3),
  ]
  puts '%-20s %s' % [base, cells.map { |c| '%10s' % c }.join(' ')]
end
