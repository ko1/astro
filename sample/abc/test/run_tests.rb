#!/usr/bin/env ruby
# frozen_string_literal: true
#
# Differential test harness for abc.
#
# Every case is fed to both `abc` and the system `bc` and their *stdout*
# is compared byte-for-byte.  (Error-message text differs between the two,
# so error cases are validated by both producing no stdout.)  In addition
# to the curated cases below we run the fixture programs under test/cases/
# and a seeded fuzzer over the arithmetic / scale rules.

require 'open3'
require 'tmpdir'

ABC = File.expand_path('../abc', __dir__)
BC  = ENV['BC'] || 'bc'

abort "abc binary not found at #{ABC} (run `make` first)" unless File.executable?(ABC)

$pass = 0
$fail = 0
$skip = 0
$failures = []

def run(cmd, input)
  out, _err, _st = Open3.capture3(*cmd, stdin_data: input)
  out
end

# Compare abc vs bc on `program`; `label` for reporting.
def diff(label, program, bc_args: [], abc_args: [])
  input = program.end_with?("\n") ? program : program + "\n"
  abc_out = run([ABC, '-q', *abc_args], input)
  bc_out  = run([BC, '-q', *bc_args], input)
  if abc_out == bc_out
    $pass += 1
  else
    $fail += 1
    $failures << [label, program, abc_out, bc_out]
  end
end

# An expected-output case (no bc); used where we intentionally define
# behaviour or bc is unavailable.
def expect(label, program, want, abc_args: [])
  input = program.end_with?("\n") ? program : program + "\n"
  got = run([ABC, '-q', *abc_args], input)
  if got == want
    $pass += 1
  else
    $fail += 1
    $failures << [label, program, got, want]
  end
end

# ---------------------------------------------------------------------
# 1. Curated cases
# ---------------------------------------------------------------------

CASES = {
  # --- basic arithmetic ---
  'add'              => '1+2',
  'precedence'       => '1+2*3-4',
  'paren'            => '(1+2)*3',
  'sub_neg'          => '5-10',
  'mul'              => '6*7',
  'int_div'          => '7/2',
  'mod'              => '7%3',
  'neg_mod'          => '-7%3',
  'mod_neg_divisor'  => '7%-3',
  'both_neg_mod'     => '-7%-3',
  'unary_minus'      => '- -5',
  'double_neg'       => '-(-5)',
  'chained_sub'      => '10-2-3',
  'big_mul'          => '99999999999*88888888888',
  'pow'              => '2^10',
  'pow_zero'         => '5^0',
  'zero_pow_zero'    => '0^0',
  'pow_big'          => '2^256',
  'pow_right_assoc'  => '2^3^2',
  'neg_base_pow'     => '-2^2',
  'pow_of_paren'     => '(-2)^3',

  # --- scale rules ---
  'div_scale0'       => '10/3',
  'div_scale2'       => 'scale=2;10/3',
  'div_scale20'      => 'scale=20;1/7',
  'mul_scale'        => 'scale=5;3*2.5',
  'mul_scale_trunc'  => 'scale=1;1.23*4.56',
  'add_scale'        => '1.5+2.25',
  'mod_scale'        => 'scale=4;10%3',
  'pow_scale'        => 'scale=4;2.5^3',
  'pow_neg_exp'      => 'scale=6;2^-3',
  'pow_frac_trunc'   => '0.5^2',
  'frac_exp_trunc'   => 'scale=4;v=4;v^0.5',
  'scale_var'        => 'scale=7;scale',
  'div_then_scale'   => 'scale=10;x=1/3;scale=2;x',

  # --- number formatting ---
  'trailing_zeros'   => '1.50',
  'leading_zeros'    => '000.500',
  'zero'             => '0',
  'zero_scaled'      => '0.000',
  'neg_zero'         => '-0',
  'pure_fraction'    => 'scale=5;1/3',
  'neg_fraction'     => 'scale=2;-1/3',
  'long_wrap'        => '2^512',
  'long_wrap_frac'   => 'scale=200;1/3',
  'big_factorialish' => '2^1000',

  # --- bases ---
  'obase_hex'        => 'obase=16;255',
  'obase_hex_big'    => 'obase=16;2^64',
  'obase_bin'        => 'obase=2;255',
  'obase_bin_frac'   => 'obase=2;10.5',
  'obase_hex_frac'   => 'obase=16;255.5',
  'obase_deadbeef'   => 'obase=16;3735928559',
  'obase_big'        => 'obase=1000;123456',
  'obase_hex_neg'    => 'obase=16;-255',
  'ibase_hex'        => 'ibase=16;FF',
  'ibase_hex_frac'   => 'ibase=16;1A.8',
  'ibase_oct'        => 'ibase=8;17',
  'ibase_2'          => 'ibase=2;101',
  'ibase_digit_face' => 'ibase=10;A',
  'base_roundtrip'   => 'obase=16;ibase=16;FF+1',

  # --- builtins ---
  'sqrt2'            => 'sqrt(2)',
  'sqrt2_scaled'     => 'scale=10;sqrt(2)',
  'sqrt_perfect'     => 'sqrt(144)',
  'sqrt0'            => 'sqrt(0)',
  'sqrt_frac'        => 'scale=5;sqrt(2.25)',
  'length_int'       => 'length(12345)',
  'length_frac'      => 'length(123.456)',
  'length_zero'      => 'length(0)',
  'length_zero_sc'   => 'length(0.00)',
  'length_small'     => 'length(0.001)',
  'scale_of'         => 'scale(123.456)',
  'scale_of_int'     => 'scale(5)',

  # --- comparison / logical ---
  'lt'               => '3<5',
  'gt'               => '3>5',
  'eq'               => '5==5',
  'ne'               => '5!=5',
  'le'               => '5<=5',
  'chain_rel'        => '1<2<3',
  'and'              => '1&&0',
  'or'               => '0||3',
  'not'              => '!0',
  'not_nonzero'      => '!5',
  'double_not'       => '!!7',
  'and_shortcircuit' => 'x=0;0&&(x=5);x',
  'or_shortcircuit'  => 'x=0;1||(x=5);x',
  'cmp_scaled'       => '1.0==1',
  'cmp_frac'         => '0.1<0.2',

  # --- variables / assignment ---
  'assign'           => 'x=5;x',
  'assign_no_print'  => 'x=5',
  'paren_assign'     => '(x=5)',
  'chained_assign'   => 'a=b=c=7;a+b+c',
  'pluseq'           => 'x=5;x+=3;x',
  'minuseq'          => 'x=5;x-=3;x',
  'stareq'           => 'x=5;x*=3;x',
  'slasheq'          => 'scale=2;x=5;x/=3;x',
  'pcteq'            => 'x=17;x%=5;x',
  'careteq'          => 'x=2;x^=10;x',
  'preinc'           => 'x=5;++x',
  'postinc'          => 'x=5;x++;x',
  'predec'           => 'x=5;--x',
  'postdec'          => 'x=5;x--;x',
  'post_in_expr'     => 'a=5;a++ + ++a',
  'undefined_var'    => 'y',
  'last_value'       => '3+4;.',
  'last_kw'          => '7*8;last',

  # --- arrays ---
  'array_basic'      => 'a[0]=10;a[1]=20;a[0]+a[1]',
  'array_default'    => 'a[5]',
  'array_expr_idx'   => 'i=2;a[i]=99;a[2]',
  'array_loop'       => 'for(i=0;i<5;i++)a[i]=i*i;a[4]',
  'array_incr'       => 'a[3]=10;a[3]++;a[3]',

  # --- control flow ---
  'if_true'          => 'if(1)5',
  'if_false_noelse'  => 'if(0)5',
  'if_else'          => 'if(0)1 else 2',
  'while_sum'        => 'i=1;s=0;while(i<=10){s+=i;i++};s',
  'for_sum'          => 's=0;for(i=1;i<=100;i++)s+=i;s',
  'nested_loops'     => 'n=0;for(i=1;i<=3;i++)for(j=1;j<=3;j++)n++;n',
  'break'            => 'i=0;while(1){i++;if(i>=5)break};i',
  'continue'         => 's=0;for(i=1;i<=10;i++){if(i%2==0)continue;s+=i};s',
  'for_empty_cond'   => 'i=0;for(;;){i++;if(i>3)break};i',
  'empty_for_parts'  => 'i=0;for(;i<3;)i++;i',

  # --- functions ---
  'func_simple'      => 'define f(x){return(x*x)};f(5)',
  'func_recursion'   => 'define f(n){if(n<2)return(1);return(n*f(n-1))};f(10)',
  'func_fib'         => 'define fib(n){if(n<2)return(n);return(fib(n-1)+fib(n-2))};fib(15)',
  'func_auto'        => 'define f(){auto i;i=99;return(i)};i=1;f();i',
  'func_no_return'   => 'define f(){5};f()',
  'func_default_ret' => 'define f(x){x+1};f(10)',
  'func_dynamic'     => "define g(){return(x)}\ndefine f(){auto x;x=42;return(g())}\nx=7\nf()",
  'func_multi_arg'   => 'define add(a,b,c){return(a+b+c)};add(1,2,3)',
  'func_scale_use'   => 'define half(x){return(x/2)};scale=3;half(1)',
  'ackermann'        => 'define a(m,n){if(m==0)return(n+1);if(n==0)return(a(m-1,1));return(a(m-1,a(m,n-1)))};a(2,3)',

  # --- print / strings ---
  'print_nums'       => 'print 1,2,3',
  'print_newline'    => 'print "a\nb\n"',
  'print_mixed'      => 'print "x=",5,"\n"',
  'bare_string'      => '"hello\n"',
  'print_expr'       => 'print 2+3,"\n"',
  'string_wrap'      => 'print "' + ('z' * 100) + '"',

  # --- comments ---
  'line_comment'     => "1+1 # comment\n2+2",
  'block_comment'    => '1 /* skip */ + 2',
  'multiline_comment'=> "x=5 /* this\nspans lines */ + 1\nx",

  # --- error recovery (both produce empty stdout) ---
  'div_zero'         => '1/0',
  'mod_zero'         => '5%0',
  'div_zero_recover' => "1/0\n42",
  'sqrt_neg'         => 'sqrt(-1)',

  # --- whitespace / formatting robustness ---
  'spaces'           => '  1   +    2  ',
  'tabs'             => "1\t+\t2",
  'multi_statement'  => '1;2;3',
  'continuation'     => "1 + \\\n2",

  # --- big computations ---
  'factorial_20'     => 'define f(n){if(n<2)return(1);return(n*f(n-1))};f(20)',
  'factorial_50'     => 'define f(n){if(n<2)return(1);return(n*f(n-1))};f(50)',
  'sum_squares'      => 's=0;for(i=1;i<=50;i++)s+=i^2;s',
  'pi_leibniz'       => 'scale=10;p=0;for(i=0;i<200;i++)p+=(-1)^i/(2*i+1);4*p',
  'power_series'     => 'scale=15;e=0;f=1;for(i=0;i<20;i++){e+=1/f;f*=(i+1)};e',

  # --- more corner cases ---
  'empty_program'    => '',
  'only_comment'     => '/* nothing */',
  'only_semicolons'  => ';;;',
  'assign_in_expr'   => 'a=3;b=(a=5)+1;a;b',
  'multi_incr'       => 'a=3;b=a++ + a++;a;b',
  'func_redefine'    => "define f(){return(1)}\ndefine f(){return(2)}\nf()",
  'deep_paren'       => '((((((1+2))))))',
  'big_obase_frac'   => 'scale=10;obase=16;1/3',
  'obase_bin_big'    => 'obase=2;2^40',
  'scale_persistence'=> 'scale=5;a=1/3;scale=2;a;a+0',
  'mod_exact'        => '100%7',
  'div_huge_scale'   => 'scale=100;355/113',
  'pow_one'          => '12345^1',
  'one_pow_huge'     => '1^1000000',
  'neg_one_pow'      => '(-1)^1001',
  'cmp_returns_num'  => 'x=(3<5);x+10',
  'nested_func_call' => "define sq(x){return(x*x)}\ndefine quad(x){return(sq(sq(x)))}\nquad(2)",
  'for_in_func'      => "define tri(n){auto s,i;s=0;for(i=1;i<=n;i++)s+=i;return(s)}\ntri(100)",
  'array_as_accum'   => "for(i=0;i<10;i++)a[i%3]+=i\na[0];a[1];a[2]",
  'while_false'      => 'while(0)1;99',
  'if_assign_cond'   => 'if(x=5)x',
  'string_then_num'  => '"val: ";42',
  'print_no_nl'      => 'print 1;print 2;print 3;"\n"',
  'negative_print'   => '-5;-5.5;-0.001',
  'huge_negative'    => '-(2^200)',
  'scale_max_of_ops' => '1.234+5.6',
  'mixed_base_calc'  => 'ibase=16;obase=10;A+A',
}

CASES.each { |name, prog| diff(name, prog) }

# ---------------------------------------------------------------------
# 2. Fixture files (test/cases/*.bc)
# ---------------------------------------------------------------------

Dir[File.join(__dir__, 'cases', '*.bc')].sort.each do |path|
  diff("fixture:#{File.basename(path)}", File.read(path))
end

# ---------------------------------------------------------------------
# 3. Seeded fuzzing of arithmetic + scale rules
# ---------------------------------------------------------------------

srand(20260528)

def rand_num
  case rand(4)
  when 0 then rand(-1000..1000).to_s
  when 1 then format('%.*f', rand(0..4), rand(-100.0..100.0))
  when 2 then rand(1..99999).to_s
  else        format('%d.%d', rand(0..999), rand(0..9999))
  end
end

OPS = %w[+ - * %]

2000.times do |i|
  scale = [0, 0, 2, 5, 10, 20].sample
  a = rand_num
  b = rand_num
  op = OPS.sample
  prog = "scale=#{scale};(#{a})#{op}(#{b})"
  diff("fuzz_arith_#{i}", prog)
end

# division separately (guard against /0)
500.times do |i|
  scale = [0, 2, 5, 10, 20].sample
  a = rand_num
  b = rand_num
  next if b.to_f == 0.0
  diff("fuzz_div_#{i}", "scale=#{scale};(#{a})/(#{b})")
end

# power with small integer exponents
300.times do |i|
  scale = [0, 2, 5, 10].sample
  base = format('%d.%d', rand(0..20), rand(0..99))
  exp = rand(0..12)
  diff("fuzz_pow_#{i}", "scale=#{scale};(#{base})^#{exp}")
end

# comparisons
300.times do |i|
  a = rand_num; b = rand_num
  op = %w[< <= > >= == !=].sample
  diff("fuzz_cmp_#{i}", "(#{a})#{op}(#{b})")
end

# compound multi-operator expressions with nesting
1000.times do |i|
  scale = [0, 2, 5, 10].sample
  terms = (2 + rand(3)).times.map { rand(3).zero? ? "(#{rand_num})" : rand_num }
  ops = (terms.size - 1).times.map { %w[+ - *].sample }
  expr = terms.first
  ops.each_with_index { |op, j| expr = "(#{expr}#{op}#{terms[j + 1]})" }
  diff("fuzz_compound_#{i}", "scale=#{scale};#{expr}")
end

# ibase round-trips: parse a value written in some base
300.times do |i|
  ib = [2, 8, 16].sample
  v = rand(0..10**rand(1..8))
  digits = v.to_s(ib).upcase
  diff("fuzz_ibase_#{i}", "ibase=#{ib};#{digits}")
end

# negative exponents at scale
200.times do |i|
  scale = [0, 5, 10, 20].sample
  base = [rand(2..9), "#{rand(1..9)}.#{rand(1..9)}"].sample
  exp = rand(1..8)
  diff("fuzz_negpow_#{i}", "scale=#{scale};(#{base})^-#{exp}")
end

# output bases
200.times do |i|
  ob = [2, 8, 16, 100, 256, 1000].sample
  v = rand(0..10**rand(1..12))
  diff("fuzz_obase_#{i}", "obase=#{ob};#{v}")
end

# output bases with fractions (exercises the ceil-digit / grouping logic)
400.times do |i|
  ob = [2, 3, 8, 16, 60, 100, 256, 1000].sample
  scale = [1, 2, 5, 10].sample
  a = rand(1..9999); b = rand(1..99)
  diff("fuzz_obase_frac_#{i}", "scale=#{scale};obase=#{ob};#{a}/#{b}")
end

# negative values across bases
150.times do |i|
  ob = [2, 16, 1000].sample
  diff("fuzz_obase_neg_#{i}", "obase=#{ob};-#{rand(1..10**6)}")
end

# ---------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------

puts
puts "passed: #{$pass}   failed: #{$fail}   skipped: #{$skip}"
unless $failures.empty?
  puts "\n=== FAILURES (first 30) ==="
  $failures.first(30).each do |label, prog, got, want|
    puts "- #{label}"
    puts "    program: #{prog.inspect}"
    puts "    abc    : #{got.inspect}"
    puts "    bc     : #{want.inspect}"
  end
end
exit($fail.zero? ? 0 : 1)
