#!/usr/bin/env ruby
# Benchmark harness for anlox (Lox on ASTro).
#
# Compares the pure interpreter against the AOT (code-store) build at ~0.5 s
# scale, verifying identical output first.  There is no system Lox to compare
# against; set ANLOX_REF=/path/to/jlox (or clox) to add a reference column.
require 'open3'
require 'tempfile'

HERE = File.dirname(File.expand_path(__FILE__))
ANLOX = ENV['ANLOX'] || File.join(HERE, '..', 'anlox')
REF   = ENV['ANLOX_REF']

PROGRAMS = {
  'fib 30'   => "fun fib(n){if(n<2)return n; return fib(n-1)+fib(n-2);} print fib(30);",
  'loop 5e6' => "fun run(n){var s=0; var i=0; while(i<n){s=s+i; i=i+1;} return s;} print run(5000000);",
}

def time(*cmd)
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  out, _e, st = Open3.capture3(*cmd)
  [out, Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0, st.exitstatus]
end

rows = []
PROGRAMS.each do |name, src|
  f = Tempfile.new(['anlox_b', '.lox']); f.write(src); f.flush
  iout, it, = time(ANLOX, f.path)
  system(ANLOX, '--aot-compile', f.path, out: File::NULL, err: File::NULL)   # warm code store
  aout, at, = time(ANLOX, '--aot-compile', f.path)
  rt = nil; rout = iout
  if REF then rout, rt, = time(REF, f.path) end
  ok = (aout == iout) && (!REF || rout == iout)
  rows << [name, it, at, rt, ok, iout.strip]
end

puts "%-10s %10s %10s %10s   %s" % ["program", "interp(s)", "aot(s)", "ref(s)", "result"]
puts "-" * 60
rows.each do |name, it, at, rt, ok, val|
  puts "%-10s %10.3f %10.3f %10s   %s (=%s)" %
       [name, it, at, rt ? ("%.3f" % rt) : "n/a", ok ? "ok" : "MISMATCH", val]
end
puts
puts "AOT folds the per-node dispatch chain; Lox's all-boxed-double numbers and"
puts "by-name global lookup dominate, so the speedup is modest (see docs/perf.md)."
