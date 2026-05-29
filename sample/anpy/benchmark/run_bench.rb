#!/usr/bin/env ruby
# frozen_string_literal: true
#
# Benchmark harness for AnPy.  Each workload is a valid ChocoPy program
# (also valid Python 3) sized for a ~1s run, executed under `python3`,
# AnPy's tree-walking interpreter, and AnPy with AOT-specialized
# dispatchers (warmed so the one-off gcc build is not timed).  Outputs are
# cross-checked so a benchmark cannot "win" by being wrong.

require 'open3'
ANPY = File.expand_path('../anpy', __dir__)
PY   = ENV['PYTHON'] || 'python3'
REPS = (ENV['REPS'] || 3).to_i
abort "anpy not built" unless File.executable?(ANPY)

B = {
  'fib_rec'   => ["def fib(n:int)->int:\n    if n<2:\n        return n\n    return fib(n-1)+fib(n-2)\nprint(fib(%d))", 31],
  'loop_sum'  => ["s:int=0\ni:int=0\nwhile i<%d:\n    s=s+i\n    i=i+1\nprint(s)", 20_000_000],
  'count_prime' => ["def isprime(n:int)->bool:\n    i:int=2\n    while i*i<=n:\n        if n%%i==0:\n            return False\n        i=i+1\n    return n>1\nc:int=0\nk:int=2\nwhile k<%d:\n    if isprime(k):\n        c=c+1\n    k=k+1\nprint(c)", 200_000],
  'collatz'   => ["c:int=0\nn:int=1\nm:int=0\nwhile n<%d:\n    m=n\n    while m>1:\n        if m%%2==0:\n            m=m//2\n        else:\n            m=3*m+1\n        c=c+1\n    n=n+1\nprint(c)", 300_000],
  'tak'       => ["def tak(x:int,y:int,z:int)->int:\n    if not (y<x):\n        return z\n    return tak(tak(x-1,y,z),tak(y-1,z,x),tak(z-1,x,y))\nprint(tak(%d,18,9))", 26],
  'method_loop' => ["class Acc(object):\n    n:int=0\n    def add(self:\"Acc\",x:int)->int:\n        self.n=self.n+x\n        return self.n\na:Acc=None\ni:int=0\nr:int=0\na=Acc()\nwhile i<%d:\n    r=a.add(i)\n    i=i+1\nprint(r)", 3_000_000],
  'list_sum'  => ["xs:[int]=None\ni:int=0\ns:int=0\ne:int=0\nxs=[0]\nwhile len(xs)<1000:\n    xs=xs+[len(xs)]\nwhile i<%d:\n    for e in xs:\n        s=s+e\n    i=i+1\nprint(s)", 3000],
}

def timed(cmd, input, reps)
  best = nil; out = nil
  reps.times do
    t = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    out, _e, _s = Open3.capture3(*cmd, stdin_data: input)
    dt = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t
    best = dt if best.nil? || dt < best
  end
  [best, out]
end

puts format('%-13s %10s %12s %12s   %8s %8s', 'benchmark', 'py3(s)', 'anpy-int(s)', 'anpy-aot(s)', 'int/py3', 'aot/int')
puts '-' * 74
ri = []; ra = []
B.each do |name, (tmpl, n)|
  prog = (tmpl % n) + "\n"
  pt, po = timed([PY], prog, REPS)
  it, io = timed([ANPY, '--plain'], prog, REPS)
  env = { 'CCACHE_DISABLE' => '1' }
  Open3.capture3(env, ANPY, '--aot-compile', stdin_data: prog)   # warm
  at, ao = timed([env, ANPY, '--aot-compile'], prog, REPS)
  ok = (po == io && io == ao)
  ri << it / pt if ok; ra << at / it if ok
  puts format('%-13s %10.3f %12.3f %12.3f   %7.2fx %7.2fx%s', name, pt, it, at, it / pt, at / it, ok ? '' : '  MISMATCH')
end
def geo(a) = a.empty? ? 0 : Math.exp(a.sum { |x| Math.log(x) } / a.size)
puts '-' * 74
puts format('%-13s %10s %12s %12s   %7.2fx %7.2fx', 'geomean', '', '', '', geo(ri), geo(ra))
puts "\nint/py3 < 1.0 → AnPy interpreter faster than CPython.  aot/int < 1.0 → AOT helps."
