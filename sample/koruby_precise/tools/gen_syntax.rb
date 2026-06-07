#!/usr/bin/env ruby
# frozen_string_literal: true
#
# gen_syntax.rb — generate golden tests for Ruby SYNTAX combinations.
#
# The method-fuzz corpus (gen_golden.rb) only ever uses one call shape:
# `(recv).meth(args)`.  Ruby has far more syntax: call forms (command, send,
# &., splat, block { } vs do/end, &:sym, numbered params, destructuring,
# block-locals), operator forms (a+b vs a.+(b) vs a.send(:+,b), [], []=, unary),
# assignment forms (multiple, splat, nested, op-assign on local/elem/ivar,
# ||= &&=), literals (numeric bases/_/exp/?c/r/i, %q %Q %w %i %W, interpolation,
# adjacency, splat-in-literal, **-merge, shorthand, symbol forms), proc/lambda
# call shapes, ranges, and one-line/expression pattern matching.
#
# Each snippet is a complete EXPRESSION (statements wrapped in `(...)`) that
# CRuby evals to a value; we emit `p (snippet)` and certify it runs cleanly,
# deterministically (no embedded address), in bounded time.
#
# Usage: ruby tools/gen_syntax.rb [OUT_DIR] [LINES_PER_FILE]
require 'timeout'

OUT   = ARGV[0] || 't/syntax'
CHUNK = (ARGV[1] || 60).to_i

def addr?(s) = s =~ /0x[0-9a-f]{3,}/i || s =~ /#</
def certify(expr)
  o1 = o2 = nil
  Timeout.timeout(1) { o1 = eval(expr).inspect; o2 = eval(expr).inspect } # rubocop:disable Security/Eval
  return nil if o1.nil? || o1 != o2 || o1.length > 200 || addr?(o1)
  expr
rescue Exception # SyntaxError (version), StandardError, etc. → drop
  nil
end

snippets = Hash.new { |h, k| h[k] = [] }
add = ->(cat, *exprs) { exprs.each { |e| (c = certify(e)) && (snippets[cat] << "p (#{c})") } }

# --- call forms: vary the SHAPE around a fixed (recv, method, args) table ---
CALLS = [
  ['"hello"', 'upcase', nil], ['"hello"', 'include?', '"ell"'], ['"a,b,c"', 'split', '","'],
  ['"hi"', 'center', '6'], ['[1,2,3]', 'first', '2'], ['[1,2,3]', 'include?', '2'],
  ['[1,2,3]', 'join', '"-"'], ['[3,1,2]', 'sort', nil], ['{a: 1}', 'fetch', ':a'],
  ['5', 'gcd', '15'], ['5', 'between?', nil], ['(1..5)', 'include?', '3'],
]
CALLS.each do |r, m, a|
  args = a ? a : nil
  paren = args ? "(#{args})" : '()'
  add.call('call', "#{r}.#{m}#{paren}")
  add.call('call', "#{r}.#{m}") if args.nil?
  add.call('call', "#{r}.#{m} #{args}") if args
  add.call('call', "#{r}.send(:#{m}#{args ? ", #{args}" : ''})")
  add.call('call', "#{r}.public_send(:#{m}#{args ? ", #{args}" : ''})")
  add.call('call', "#{r}.method(:#{m}).call#{paren}")
  add.call('call', "#{r}&.#{m}#{paren}")
  add.call('call', "#{r}.#{m}(*[#{args}])") if args
  add.call('call', "#{r}.then { |x| x.#{m}#{paren} }")
  add.call('call', "nil&.#{m}")
end

# --- block forms ---
add.call('block',
  '[1,2,3].map { |x| x * 2 }',
  '[1,2,3].map do |x| x * 2 end',
  '[1,2,3].map { _1 * 2 }',
  '[1,2,3].map { it * 2 }',
  '[1,2,3].each_with_index.map { |x, i| [i, x] }',
  '[[1,2],[3,4]].map { |a, b| a + b }',
  '[[1,2],[3,4]].map { |(a, b)| a - b }',
  '{a: 1, b: 2}.map { |k, v| [k, v] }',
  '[1,2,3].map(&:to_s)',
  '[1,2,3].select(&:odd?)',
  '[1,2,3].reduce(:+)',
  '[1,2,3].each_with_object([]) { |x, acc| acc << x }',
  '[1,2,3].map { |x| y = x * x; y + 1 }',
  '[1,2,3].inject { |a, b| a + b }',
  '(1..3).map { |x| x ** 2 }',
  '%w[a b c].each_with_index.to_a',
  '[1,2,3,4].each_slice(2).map { |a, b| a + b }')

# --- operator forms (a op b vs a.op(b) vs send) ---
OPS = [['5', '+', '3'], ['10', '-', '4'], ['6', '*', '7'], ['17', '/', '5'], ['17', '%', '5'],
       ['2', '**', '8'], ['5', '<=>', '3'], ['5', '==', '5'], ['5', '<', '6'], ['5', '>=', '5'],
       ['"a"', '+', '"b"'], ['"ab"', '*', '3'], ['[1]', '+', '[2]'], ['[1,2,3]', '-', '[2]'],
       ['5', '&', '3'], ['5', '|', '2'], ['5', '^', '1'], ['1', '<<', '4'], ['256', '>>', '2']]
OPS.each do |a, op, b|
  add.call('operator', "#{a} #{op} #{b}", "#{a}.#{op}(#{b})", "#{a}.send(:#{op}, #{b})")
end
add.call('operator',
  '[10,20,30][1]', '[10,20,30].[](1)', '[10,20,30][-1]', '[10,20,30][1, 2]', '[10,20,30][1..2]',
  '(a = [0,0,0]; a[1] = 9; a)', '(a = [0,0,0]; a.[]=(1, 9); a)',
  '"hello"[1]', '"hello"[1, 3]', '"hello"[1..3]', '"hello"[-1]',
  '({a: 1}[:a])', '(h = {}; h[:x] = 5; h)',
  '-5', '-(3 + 2)', '5.-@', '!true', 'true.!', '!!nil', '!nil',
  '5 == 5.0', '5.eql?(5)', '5.equal?(5)', '1 <=> 2', '"a" <=> "b"',
  '1 == 1 && 2 == 2', '1 == 2 || 3 == 3', 'not false', '1 and 2', 'nil or 5',
  '(1..5) === 3', 'Integer === 5', 'String === "x"',
  '5.clamp(1, 3)', '5.between?(1, 10)')

# --- assignment forms (statements wrapped as expressions) ---
add.call('assign',
  '(x = 5; x)',
  '(x = y = z = 7; [x, y, z])',
  '(a, b = 1, 2; [a, b])',
  '(a, b = 1, 2; a, b = b, a; [a, b])',
  '(a, b, c = [10, 20, 30]; [a, b, c])',
  '(first, *rest = 1, 2, 3, 4; [first, rest])',
  '(*init, last = 1, 2, 3, 4; [init, last])',
  '(a, *mid, z = 1, 2, 3, 4, 5; [a, mid, z])',
  '(a, (b, c), d = 1, [2, 3], 4; [a, b, c, d])',
  '(x = 5; x += 3; x)', '(x = 5; x -= 2; x)', '(x = 5; x *= 2; x)',
  '(x = 20; x /= 3; x)', '(x = 20; x %= 7; x)', '(x = 2; x **= 5; x)',
  '(x = 1; x <<= 4; x)', '(x = 12; x >>= 2; x)',
  '(x = 5; x |= 2; x)', '(x = 7; x &= 3; x)', '(x = 5; x ^= 1; x)',
  '(x = nil; x ||= 9; x)', '(x = 3; x ||= 9; x)',
  '(x = 1; x &&= x + 1; x)', '(x = nil; x &&= 5; x)',
  '(s = "a"; s += "b"; s *= 2; s)',
  '(a = [0, 0]; a[0] += 10; a)', '(a = [1]; a[0] *= 3; a)',
  '(h = {x: 1}; h[:x] += 5; h)', '(h = Hash.new(0); h[:k] += 1; h[:k] += 1; h)',
  '(a = [1, 2, 3]; a[1..2] = [9]; a)',
  '(x, = [1, 2]; x)',
  '(arr = [*1..3, *5..6]; arr)')

# --- literal forms ---
add.call('literal',
  '1_000_000', '0xff', '0b1010', '0o17', '0d42', '1e3', '1.5e-2', '12_34.56_78',
  '?A', '?z', '?\n'.dump.then { '?\n' }, '1r', '3i', '1.5r', '(2 + 3i)', '(1/3r)',
  '%q(no interp #{1})', '%Q(interp #{1 + 1})', '"adj" "acent"', "'single'",
  '"tab\\tend"', '"\\u0041\\u0042"', '"#{1}#{2}#{3}"', '"nested #{"in #{1 + 1}"}"',
  '%w[a b c]', '%i[x y z]', '%W[a#{1} b#{2}]', '%i[a b].map(&:to_s)',
  '[*[1, 2], 3, *[4, 5]]', '[1, *(2..4), 5]', '[*"a".."c"]',
  '{**{a: 1}, b: 2}', '{a: 1, **{b: 2, c: 3}}', '(x = 1; y = 2; {x:, y:})',
  '{ "key" => "val", sym: 1, 3 => "three" }',
  ':+', ':[]', ':[]=', ':"a b"', ':"x#{1 + 1}"', '%i[plus minus]',
  '(1..)', '(..5)', '(1..).first(3)', '(..5).include?(3)',
  '->(x) { x + 1 }.call(10)', '->(x) { x }.(5)', '->(x) { x * 2 }[7]',
  '->() { 42 }.call', '->(a, b = 2) { a + b }.(1)', '->(*a) { a }.(1, 2, 3)',
  '->(a:, b: 10) { a + b }.(a: 1)', '->(a, *b, c:) { [a, b, c] }.(1, 2, 3, c: 4)',
  'lambda { |a, b| a + b }.call(2, 3)', 'proc { |x| x * x }.call(6)',
  'def (obj = Object.new).hi; 1; end; obj.hi'.then { '(obj = Object.new; def obj.hi; 42; end; obj.hi)' },
  'begin; 1 + 1; end', 'true ? "y" : "n"', '(1 > 0 ? :a : :b)',
  '(5.times.to_a)', '(3.downto(1).to_a)')

# --- expression-form pattern matching & one-line constructs ---
add.call('pattern',
  '(case [1, 2] in [a, b] then [a, b] end)',
  '(case [1, 2, 3] in [1, *rest] then rest end)',
  '(case {name: "x", age: 3} in {name:, age:} then [name, age] end)',
  '(case {a: 1} in {a: Integer => n} then n end)',
  '(case 5 in Integer => x then x end)',
  '(case [1, [2, 3]] in [a, [b, c]] then [a, b, c] end)',
  '(case "x" in String | Symbol then :ok end)',
  '(x = 3; case [1, 2, 3] in [_, ^x, _] then :no else :yes end)',
  '([1, 2] in [a, b])',
  '({a: 1, b: 2} in {a: Integer})',
  '(1 => x; x)',
  '(case 10 in 0..5 then :low in 6..15 then :mid end)',
  '(case [1, 2, 3, 4, 5] in [*, 3, *post] then post end)',
  '(if x = 5 then x end)',
  '(y = 10 if true; y)',
  '(z = (1..3).map { |i| i * i }; z)',
  '(v = [1, 2, 3].sum > 5 ? :big : :small; v)',
  '((1 rescue 2))', '((raise "e" rescue :saved))',
  '(x = 0; x += 1 while x < 5; x)',
  '(x = 10; x -= 1 until x <= 5; x)',
  '(acc = []; for i in 1..3; acc << i; end; acc)',
  '(acc = []; for k, v in {a: 1, b: 2}; acc << [k, v]; end; acc)')

# --- argument-binding matrix: def-signature × call-args (the big combinatoric) ---
REQ  = ['', 'a', 'a, b']
OPT  = ['', 'o = 10']
REST = ['', '*r']
POST = ['', 'z']
KW   = ['', 'k:', 'k: 99']
KWR  = ['', '**kw']
CALLARGS = ['', '1', '1, 2', '1, 2, 3', '1, 2, 3, 4', '*[1, 2]', '*[1, 2, 3]', '1, *[2, 3]',
            'k: 5', '1, k: 5', '1, 2, k: 5', '1, **{k: 5}', '**{k: 5}',
            '1, 2, 3, k: 5, extra: 6', '*[1, 2], k: 5', '1, **{k: 5, z: 9}']
sig_count = 0
REQ.each do |rq|
 OPT.each do |op|
  REST.each do |rs|
   POST.each do |po|
    next if po != '' && rs == '' # post requires rest
    KW.each do |kw|
     KWR.each do |kr|
      parts = [rq, op, rs, po, kw, kr].reject(&:empty?)
      next if parts.empty?
      names = []
      names << 'a' if rq.include?('a'); names << 'b' if rq.include?('b')
      names << 'o' if op != ''
      names << 'r' if rs != ''
      names << 'z' if po != ''
      names << 'k' if kw != ''
      names << 'kw' if kr != ''
      sig = parts.join(', ')
      body = "[#{names.join(', ')}]"
      sig_count += 1
      CALLARGS.each do |ca|
        snip = "(def m(#{sig}); #{body}; end; m(#{ca}))"
        (c = certify(snip)) && (snippets['args'] << "p (#{c})")
      end
     end
    end
   end
  end
 end
end
warn "  (#{sig_count} signatures fuzzed against #{CALLARGS.size} call-arg forms)"

require 'fileutils'
FileUtils.mkdir_p(OUT)
total = files = 0
snippets.each do |cat, ls|
  ls.uniq!
  ls.each_slice(CHUNK).with_index do |slice, i|
    File.write(File.join(OUT, format('%s_%03d.rb', cat, i)),
               "# generated syntax golden: #{cat} ##{i} (oracle = CRuby)\n" + slice.join("\n") + "\n")
    files += 1
    total += slice.size
  end
  warn format('  %-9s %4d', cat, ls.size)
end
warn format('TOTAL %d syntax assertions across %d files', total, files)
