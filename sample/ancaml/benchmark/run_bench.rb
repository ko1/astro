#!/usr/bin/env ruby
# Benchmark harness for ancaml (MinCaml on ASTro).
#
# Compares, at ~1 s scale, four ways of running the same MinCaml program:
#   ancaml (interp)   — pure tree-walking interpreter
#   ancaml (AOT)      — ASTro code-store specialized dispatchers (--aot-compile)
#   ocaml           — OCaml bytecode toplevel (reference interpreter)
#   ocamlopt        — OCaml native code (reference compiler), if available
#
# Outputs are verified equal before timing.  Set ANCAML=/path to override.
require 'open3'
require 'tempfile'
require 'benchmark'

HERE = File.dirname(File.expand_path(__FILE__))
ANCAML = ENV['ANCAML'] || File.join(HERE, '..', 'ancaml')
PRELUDE = <<~ML
  let print_char n = print_char (Char.chr n)
  module Array = struct include Stdlib.Array let create = make end
ML

# name => MinCaml source (sized for ~1 s in the interpreter).
PROGRAMS = {
  'fib 35'   => 'let rec fib n = if n <= 1 then n else fib (n-1) + fib (n-2) in print_int (fib 35); print_newline ()',
  'ffib 34'  => 'let rec ffib n = if n <= 1 then 1.0 else ffib (n-1) +. ffib (n-2) in print_int (int_of_float (ffib 34)); print_newline ()',
  'ack 3 9'  => 'let rec ack x y = if x <= 0 then y+1 else if y <= 0 then ack (x-1) 1 else ack (x-1) (ack x (y-1)) in print_int (ack 3 9); print_newline ()',
}

def time_cmd(*cmd, stdin: nil)
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  out, _err, st = Open3.capture3(*cmd, stdin_data: stdin || '')
  t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  [out, t1 - t0, st.exitstatus]
end

def ocac_file(src)
  f = Tempfile.new(['ancaml_b', '.ml'])
  f.puts PRELUDE
  f.puts 'let _ = ('
  f.puts src
  f.puts ')'
  f.flush
  f
end

have_ocamlopt = system('which ocamlopt > /dev/null 2>&1')

rows = []
PROGRAMS.each do |name, src|
  # write program file for ancaml
  prog = Tempfile.new(['ancaml_p', '.ml']); prog.write(src); prog.flush

  interp_out, interp_t, = time_cmd(ANCAML, prog.path)
  # warm the code store, then time the AOT run
  system(ANCAML, '--aot-compile', prog.path, out: File::NULL, err: File::NULL, in: File::NULL)
  aot_out, aot_t, = time_cmd(ANCAML, '--aot-compile', prog.path)

  of = ocac_file(src)
  ml_out, ml_t, = time_cmd('ocaml', of.path)

  opt_t = nil; opt_out = interp_out
  if have_ocamlopt
    Dir.mktmpdir do |d|
      src_ml = File.join(d, 'b.ml')
      File.write(src_ml, File.read(of.path))
      if system("ocamlopt -O3 -o #{d}/b #{src_ml} 2>/dev/null") || system("ocamlopt -o #{d}/b #{src_ml} 2>/dev/null")
        opt_out, opt_t, = time_cmd("#{d}/b")
      end
    end
  end

  ok = [aot_out, ml_out, opt_out].all? { |o| o == interp_out }
  rows << [name, interp_t, aot_t, ml_t, opt_t, ok, interp_out.strip]
end

puts "%-10s %10s %10s %10s %10s   %s" % %w[program interp(s) aot(s) ocaml(s) ocamlopt(s) result]
puts '-' * 72
rows.each do |name, it, at, mt, ot, ok, val|
  puts "%-10s %10.3f %10.3f %10.3f %10s   %s (=%s)" %
       [name, it, at, mt, ot ? ('%.3f' % ot) : 'n/a', ok ? 'ok' : 'MISMATCH', val]
end
puts
puts "interp/AOT/ocaml(bytecode) are all tree/bytecode interpreters; ocamlopt is native."
