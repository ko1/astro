#!/usr/bin/env ruby
# frozen_string_literal: true
#
# Benchmark harness for abc.
#
# Each workload is sized to run for roughly a second so timings reflect
# steady-state interpretation, not process startup.  We time the system
# `bc`, abc's tree-walking interpreter, and abc with AOT-specialized
# dispatchers (warmed once so the one-off gcc build is not counted).
# Outputs are cross-checked so a benchmark can never "win" by being wrong.

require 'open3'

ABC = File.expand_path('../abc', __dir__)
BC  = ENV['BC'] || 'bc'
REPS = (ENV['REPS'] || 3).to_i

abort "abc not built (run `make`)" unless File.executable?(ABC)

# N is substituted into each program; sized for ~1s workloads.
BENCHMARKS = {
  'int_sum'      => ['s=0;for(i=1;i<=%d;i++)s+=i;s',                                   3_000_000],
  'fib_rec'      => ["define fib(n){if(n<2)return(n);return(fib(n-1)+fib(n-2))}\nfib(%d)", 30],
  'factorial'    => ['f=1;for(i=1;i<=%d;i++)f*=i;length(f)',                            30_000],
  'div_scale'    => ['scale=40;s=0;for(i=1;i<=%d;i++)s+=1/i;s',                         400_000],
  'modpow_loop'  => ['a=1;for(i=1;i<=%d;i++)a=(a*7)%%1000003;a',                        800_000],
  'sqrt_loop'    => ['scale=20;x=0;for(i=1;i<=%d;i++)x+=sqrt(i);x',                     500_000],
  'pi_leibniz'   => ['scale=30;p=0;for(i=0;i<%d;i++)p+=(-1)^i/(2*i+1);4*p',            400_000],
  'collatz'      => ["c=0;for(n=1;n<=%d;n++){m=n;while(m>1){if(m%%2)m=3*m+1 else m/=2;c+=1}};c", 30_000],
}

def time_cmd(cmd, input, reps)
  best = nil
  out = nil
  reps.times do
    t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    out, _err, _st = Open3.capture3(*cmd, stdin_data: input)
    dt = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
    best = dt if best.nil? || dt < best
  end
  [best, out]
end

puts format('%-14s %10s %12s %12s   %8s %8s', 'benchmark', 'bc(s)', 'abc-int(s)', 'abc-aot(s)', 'int/bc', 'aot/int')
puts '-' * 76

ratios_int = []
ratios_aot = []

BENCHMARKS.each do |name, (tmpl, n)|
  prog = (tmpl % n) + "\n"

  bc_t, bc_out = time_cmd([BC, '-q'], prog, REPS)
  int_t, int_out = time_cmd([ABC, '-q', '--plain'], prog, REPS)

  # warm the AOT code store (one-off gcc build not timed), then measure.
  env = { 'CCACHE_DISABLE' => '1' }
  Open3.capture3(env, ABC, '-q', '--aot-compile', stdin_data: prog)
  aot_t, aot_out = time_cmd([env, ABC, '-q', '--aot-compile'], prog, REPS)

  ok = (bc_out == int_out && int_out == aot_out)
  flag = ok ? '' : '  <-- MISMATCH'

  ratios_int << (int_t / bc_t) if ok
  ratios_aot << (aot_t / int_t) if ok

  puts format('%-14s %10.3f %12.3f %12.3f   %7.2fx %7.2fx%s',
              name, bc_t, int_t, aot_t, int_t / bc_t, aot_t / int_t, flag)
  unless ok
    puts "    bc : #{bc_out.inspect[0, 70]}"
    puts "    int: #{int_out.inspect[0, 70]}"
    puts "    aot: #{aot_out.inspect[0, 70]}"
  end
end

def geomean(a)
  return 0 if a.empty?
  Math.exp(a.sum { |x| Math.log(x) } / a.size)
end

puts '-' * 76
puts format('%-14s %10s %12s %12s   %7.2fx %7.2fx', 'geomean', '', '', '',
            geomean(ratios_int), geomean(ratios_aot))
puts
puts "int/bc  < 1.0 means abc's interpreter is faster than bc."
puts "aot/int < 1.0 means AOT specialization speeds abc up."
