#!/usr/bin/env ruby
# Run all benchmarks and compare against node (if installed) and v8/d8 (if installed).

BENCHES = %w[fib fact sieve mandelbrot nbody binary_trees]
ENGINES = []

ENGINES << ['jstro', './jstro']
ENGINES << ['node',   'node']    if system('which node > /dev/null 2>&1')
ENGINES << ['d8',     'd8']      if system('which d8 > /dev/null 2>&1')
ENGINES << ['qjs',    'qjs']     if system('which qjs > /dev/null 2>&1')  # QuickJS

trials = (ENV['TRIALS'] || 1).to_i

results = {}

BENCHES.each do |b|
  results[b] = {}
  ENGINES.each do |(name, cmd)|
    times = []
    trials.times do
      out = `#{cmd} benchmark/#{b}.js 2>&1`
      if (m = out.match(/elapsed[\s:=]+([0-9.eE+-]+)\s*s/))
        times << m[1].to_f
      end
    end
    next if times.empty?
    best = times.min
    results[b][name] = best
  end
end

printf "%-15s", "bench"
ENGINES.each { |(name,_)| printf "%12s", name }
puts
puts "-" * (15 + 12 * ENGINES.length)
results.each do |b, ts|
  printf "%-15s", b
  ENGINES.each do |(name,_)|
    if ts[name]
      printf "%12.3f", ts[name]
    else
      printf "%12s", "-"
    end
  end
  puts
end
