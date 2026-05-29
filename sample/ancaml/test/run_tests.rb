#!/usr/bin/env ruby
# Differential test harness for ancaml (MinCaml on ASTro).
#
# MinCaml is a (near-)subset of OCaml, so we check ancaml against the system
# `ocaml` toplevel: a tiny prelude reconciles the two surface differences —
# `print_char` takes an int in MinCaml (a char in OCaml) and `Array.create`
# was removed from modern OCaml (use `Array.make`).  Every well-typed program
# is run both ways and stdout is compared.
#
# Three groups:
#   1. positive   — output of ancaml must equal output of ocaml
#   2. type-error — ancaml must statically REJECT (ocaml accepts the same text)
#   3. fixtures   — test/cases/*.ml, compared against ocaml
#
# Override the interpreter with ANCAML=/path/to/ancaml (defaults to ../ancaml).
require 'open3'
require 'tempfile'

HERE = File.dirname(File.expand_path(__FILE__))
ANCAML = ENV['ANCAML'] || File.join(HERE, '..', 'ancaml')
OCAML = ENV['OCAML'] || 'ocaml'

PRELUDE = <<~ML
  let print_char n = print_char (Char.chr n)
  module Array = struct include Stdlib.Array let create = make end
ML

$pass = 0
$fail = 0

def run_ancaml(src)
  out, _err, st = Open3.capture3(ANCAML, stdin_data: src)
  [out, st.exitstatus]
end

def run_ocaml(src)
  Tempfile.create(['ancaml_t', '.ml']) do |f|
    f.puts PRELUDE
    f.puts 'let _ = ('
    f.puts src
    f.puts ')'
    f.flush
    out, _err, _st = Open3.capture3(OCAML, f.path)
    return out
  end
end

def positive(name, src)
  got, st = run_ancaml(src)
  want = run_ocaml(src)
  if st == 0 && got == want
    $pass += 1
    # puts "ok   #{name}"
  else
    $fail += 1
    puts "FAIL #{name}"
    puts "  ancaml  (exit #{st}): #{got.inspect}"
    puts "  ocaml          : #{want.inspect}"
  end
end

def type_error(name, src)
  _got, st = run_ancaml(src)
  if st != 0
    $pass += 1
  else
    $fail += 1
    puts "FAIL #{name} — expected a static type error, but ancaml accepted it"
  end
end

# `--dump-types` prints `- : <type>` (then runs the program).  We compare the
# first line against the expected inferred type.
def type_test(name, src, expected)
  out, _err, st = Open3.capture3(ANCAML, '--dump-types', stdin_data: src)
  got = out.lines.first.to_s.chomp
  if st == 0 && got == "- : #{expected}"
    $pass += 1
  else
    $fail += 1
    puts "FAIL #{name} (exit #{st})"
    puts "  got:  #{got.inspect}"
    puts "  want: #{"- : #{expected}".inspect}"
  end
end

# ---- 1. positive programs -------------------------------------------------

positive 'int arith',      'print_int (1 + 2 - 3 + 100)'
positive 'unary minus',    'print_int (- (3 + 4))'
positive 'fib',            'let rec fib n = if n <= 1 then n else fib (n-1) + fib (n-2) in print_int (fib 20)'
positive 'ackermann',      'let rec ack x y = if x <= 0 then y+1 else if y <= 0 then ack (x-1) 1 else ack (x-1) (ack x (y-1)) in print_int (ack 2 7)'
positive 'gcd (subtract)', 'let rec gcd a b = if a = b then a else if a < b then gcd a (b - a) else gcd (a - b) b in print_int (gcd 36 24)'
positive 'let chain',      'let a = 1 in let b = a + 10 in let c = a + b in print_int c'
positive 'bool/compare',   'print_int (if 3 < 5 then 1 else 0); print_int (if 5 <= 5 then 1 else 0); print_int (if 6 <> 6 then 1 else 0); print_int (if 9 > 2 then 1 else 0); print_int (if 2 >= 9 then 1 else 0)'
positive 'float arith',    'print_int (int_of_float (3.0 *. 2.5 +. 1.0))'
positive 'float fns',      'print_int (int_of_float (sqrt 144.0)); print_int (truncate (floor 3.9)); print_int (int_of_float (abs_float (-. 5.0)))'
positive 'float<->int',    'print_int (int_of_float (float_of_int 7 +. 0.5))'
positive 'tuple',          'let rec mk x = (x, x + 1, x + 2) in let (a, b, c) = mk 10 in print_int a; print_int b; print_int c'
positive 'array sum',      'let a = Array.create 5 0 in let rec set i = if i >= 5 then () else (a.(i) <- i + i; set (i+1)) in set 0; let rec sum i = if i >= 5 then 0 else a.(i) + sum (i+1) in print_int (sum 0)'
positive 'higher order',   'let rec app f x = f x in let rec inc n = n + 1 in print_int (app inc 41)'
positive 'closure capture','let rec adder n = let rec add m = n + m in add in let f = adder 10 in print_int (f 5); print_int (f 100)'
positive 'seq + newline',  'print_int 1; print_newline (); print_int 2; print_newline ()'
positive 'nested if',      'let rec sign x = if x < 0 then -1 else if x = 0 then 0 else 1 in print_int (sign (-9)); print_int (sign 0); print_int (sign 4)'
positive '5-ary fn',       'let rec f a b c d e = a + b + c + d + e in print_int (f 1 2 3 4 5)'
positive 'float compare',  'print_int (if 1.5 <= 2.5 then 1 else 0); print_int (if 3.0 < 1.0 then 1 else 0)'
positive 'tuple eq',       'print_int (if (1, 2) = (1, 2) then 1 else 0); print_int (if (1, 2) = (1, 3) then 1 else 0)'
positive 'print_float',    'print_float 3.14; print_newline (); print_float 1.0; print_newline (); print_float (2.0 /. 3.0); print_newline (); print_float (sqrt 2.0)'
positive 'tail loop big',  'let rec loop i acc = if i <= 0 then acc else loop (i - 1) (acc + i) in print_int (loop 1000000 0)'

# ---- 2. programs ancaml must statically reject ------------------------------

type_error 'int + float',     'print_int (1 + 2.0)'
type_error 'if non-bool',     'print_int (if 3 then 1 else 0)'
type_error 'branch mismatch', 'let x = if true then 1 else 2.0 in print_int 0'
type_error 'arity mismatch',  'let rec f x = x in print_int (f 1 2)'
type_error 'float op on int', 'print_int (int_of_float (1 *. 2))'
type_error 'unbound var',     'print_int y'
type_error 'array elem type', 'let a = Array.create 3 0 in a.(0) <- 1.0; print_int 0'

# ---- 2b. inferred-type golden checks (no runtime output) ------------------

type_test 'ty int',       '1 + 2', 'int'
type_test 'ty bool',      '3 <= 4', 'bool'
type_test 'ty float',     '1.0 *. 2.0', 'float'
type_test 'ty unit',      'print_int 1', 'unit'
type_test 'ty fn',        'let rec f x = x + 1 in f', '(int -> int)'
type_test 'ty poly→int',  'let rec id x = x in id', '(int -> int)'   # unresolved var defaults to int
type_test 'ty higher',    'let rec twice f x = f (f x) in twice', '((int -> int) -> int -> int)'
type_test 'ty tuple',     '(1, 2.0, 3 <= 4)', '(int * float * bool)'
type_test 'ty array',     'Array.create 3 0', 'int array'

# ---- 3. fixtures ----------------------------------------------------------

Dir[File.join(HERE, 'cases', '*.ml')].sort.each do |path|
  src = File.read(path)
  positive "cases/#{File.basename(path)}", src
end

puts
puts "ancaml tests: #{$pass} passed, #{$fail} failed"
exit($fail == 0 ? 0 : 1)
