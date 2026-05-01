#!/usr/bin/env ruby
# Run all benchmarks and compare against node / d8 / qjs.
#
# jstro modes measured (each is a separate column):
#
#   jstro         pure tree-walking interpreter (--no-compile)
#   jstro-c       AOT-bake-then-run in a single invocation: this column
#                 includes the bake cost.  Lower bound on first-run latency.
#   jstro-cached  AOT-bake first (--aot-compile, exits), then time a fresh
#                 invocation that loads the cached all.so at startup.
#                 Approximates steady-state (warm) performance — no JIT,
#                 the SDs are precompiled native code dlopen'd at init.

BENCHES = %w[fib fact sieve mandelbrot nbody binary_trees]
ENGINES = [
  ['jstro',        :plain],
  ['jstro-c',      :aot_first],
  ['jstro-cached', :aot_cached],
]

EXTERNAL = []
EXTERNAL << ['node', 'node'] if system('which node > /dev/null 2>&1')
EXTERNAL << ['d8',   'd8']   if system('which d8 > /dev/null 2>&1')
EXTERNAL << ['qjs',  'qjs']  if system('which qjs > /dev/null 2>&1')

trials = (ENV['TRIALS'] || 1).to_i

def run_jstro(mode, file)
  case mode
  when :plain
    `rm -rf code_store`
    `CCACHE_DISABLE=1 ./jstro --no-compile #{file} 2>&1`
  when :aot_first
    `rm -rf code_store`
    `CCACHE_DISABLE=1 ./jstro -c #{file} 2>&1`
  when :aot_cached
    `rm -rf code_store`
    `CCACHE_DISABLE=1 ./jstro --aot-compile #{file} 2>&1`
    `CCACHE_DISABLE=1 ./jstro #{file} 2>&1`
  end
end

results = {}

BENCHES.each do |b|
  results[b] = {}
  ENGINES.each do |(name, mode)|
    times = []
    trials.times do
      out = run_jstro(mode, "benchmark/#{b}.js")
      if (m = out.match(/elapsed[\s:=]+([0-9.eE+-]+)\s*s/))
        times << m[1].to_f
      end
    end
    results[b][name] = times.min unless times.empty?
  end
  EXTERNAL.each do |(name, cmd)|
    times = []
    trials.times do
      out = `#{cmd} benchmark/#{b}.js 2>&1`
      if (m = out.match(/elapsed[\s:=]+([0-9.eE+-]+)\s*s/))
        times << m[1].to_f
      end
    end
    results[b][name] = times.min unless times.empty?
  end
end

cols = ENGINES.map(&:first) + EXTERNAL.map(&:first)
printf "%-15s", "bench"
cols.each { |name| printf "%14s", name }
puts
puts "-" * (15 + 14 * cols.length)
results.each do |b, ts|
  printf "%-15s", b
  cols.each do |name|
    if ts[name]
      printf "%14.3f", ts[name]
    else
      printf "%14s", "-"
    end
  end
  puts
end
