#!/usr/bin/env ruby
# aforth benchmark runner.  Modeled on sample/koruby/benchmark/run.rb.
#
# Usage:
#   ruby benchmark/run.rb               # run all benches, all runners
#   ruby benchmark/run.rb fib ack       # subset
#   ruby benchmark/run.rb -n 5          # best-of-N (default 3)
#   ruby benchmark/run.rb -r interp     # only the interp runner

require 'optparse'

AFORTH_DIR = File.expand_path('..', __dir__)
BENCH_DIR  = __dir__
STORE      = "#{AFORTH_DIR}/code_store"
AFORTH     = "#{AFORTH_DIR}/aforth"

# AOT setup is per-benchmark: clear code_store, run --aot-compile to populate
# code_store/all.so, then time `aforth file.fs`.  Set CCACHE_DISABLE=1 because
# this sandbox restricts ccache's tmp directory; the env survives only inside
# the Ruby script (the produced all.so is fine to use without ccache after).
RUNNERS = [
  { name: 'interp',
    cmd:  "#{AFORTH} -q --no-compile %s" },
  { name: 'aforth+aot',
    cmd:  "#{AFORTH} -q %s",
    setup: proc { |bench|
      system("rm -rf #{STORE}")
      system({'CCACHE_DISABLE' => '1'},
             "#{AFORTH} -q --aot-compile #{bench} >/dev/null 2>&1")
    }},
  { name: 'gforth',
    cmd:  'gforth %s -e bye',
    available: -> { system('which gforth >/dev/null 2>&1') } },
]

def time_run(cmd)
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  `#{cmd} 2>&1`
  t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  t1 - t0
end

def main(argv)
  iters = 3
  runner_filter = nil
  warmup = true
  OptionParser.new do |o|
    o.on('-n N', Integer, 'iterations (best-of-N)') { |n| iters = n }
    o.on('-r R', String, 'comma-separated runner names') { |r| runner_filter = r.split(',') }
    o.on('-l', 'list benches and exit') {
      Dir["#{BENCH_DIR}/bm_*.fs"].sort.each { |f| puts File.basename(f, '.fs').sub('bm_', '') }
      exit
    }
    o.on('--no-warmup') { warmup = false }
  end.parse!(argv)

  benches = Dir["#{BENCH_DIR}/bm_*.fs"].sort
  unless argv.empty?
    benches = benches.select { |f| argv.include?(File.basename(f, '.fs').sub('bm_', '')) }
  end

  runners = RUNNERS.select { |r|
    next false if runner_filter && !runner_filter.include?(r[:name])
    r[:available] ? r[:available].call : true
  }

  unless File.executable?(AFORTH)
    abort "aforth binary not built; run `make` in #{AFORTH_DIR}"
  end

  printf "%-20s", 'bench (s)'
  runners.each { |r| printf " %14s", r[:name] }
  puts
  puts '-' * (20 + 15 * runners.size)

  benches.each do |bench|
    name = File.basename(bench, '.fs').sub('bm_', '')
    printf "%-20s", name
    runners.each do |r|
      r[:setup]&.call(bench)
      `#{r[:cmd] % bench} >/dev/null 2>&1` if warmup
      best = nil
      iters.times do
        t = time_run(r[:cmd] % bench)
        best = t if best.nil? || t < best
      end
      printf " %14.3f", best
    end
    puts
  end
end

main(ARGV)
