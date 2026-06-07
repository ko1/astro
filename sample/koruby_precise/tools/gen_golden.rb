#!/usr/bin/env ruby
# frozen_string_literal: true
#
# gen_golden.rb — generate a LARGE golden-test corpus for the koruby rebuild.
#
# Strategy: reflect every public method of each core class, fuzz arguments from
# a small curated pool, and keep only calls that (on CRuby) run without error,
# in bounded time, and produce DETERMINISTIC output (no embedded addresses).
# Each kept call is emitted as a self-contained `p (<literal>).m(args)` line so
# every assertion reconstructs its own receiver (no cross-line mutation).
# The golden answer is whatever CRuby prints at diff time; here we only certify
# that the line is valid + deterministic, then chunk into small files so one
# interpreter crash loses only ~80 assertions, not the whole class.
#
# Usage: ruby tools/gen_golden.rb [OUT_DIR] [LINES_PER_FILE]
require 'timeout'

OUT   = ARGV[0] || 't/method'
CHUNK = (ARGV[1] || 80).to_i

# Receiver literals per category (kept small so output stays bounded).
RECV = {
  'integer'  => %w[0 1 -1 2 3 4 5 7 8 9 10 16 42 50 64 99 100 128 255 256 1000 1024
                   -2 -7 -10 -100 12345 -1000000 1000000 65536],
  'float'    => %w[0.0 1.0 -1.0 1.5 -2.5 3.14 -3.14 2.0 100.0 0.5 0.25 0.333 1.25
                   10.0 1000.0 0.001 2.5 -0.5],
  'string'   => ['""', '"a"', '"hello"', '"Hello World"', '"abc"', '"a,b,c"', '"  pad  "',
                 '"123"', '"aAbBcC"', '"hello world foo"', '"x-y-z"', '"RUBY"', '"  "',
                 '"a b c d"', '"Hello, World!"', '"012345"', '"aaa"', '"The quick brown fox"',
                 '"CamelCase"', '"snake_case_name"', '"\\ttab\\n"', '"repeat repeat"'],
  'symbol'   => [':a', ':hello', ':abc', ':"with space"', ':Z', ':foo', ':Bar', ':a1', ':"123"'],
  'array'    => ['[]', '[1]', '[1,2,3]', '[3,1,2]', '[1,2,2,3,3]', '[1,[2,[3]]]', '["a","b","c"]',
                 '[1,nil,2,nil]', '[5,4,3,2,1]', '[1,2,3,4,5,6]', '[10,20,30,40]', '["x"]',
                 '[true,false,nil]', '[1.0,2.0,3.0]', '[[1],[2],[3]]', '[1,1,1]', '[2,4,6,8]',
                 '[9,8,7,6,5,4,3,2,1]', '[[1,2],[3,4]]', '["b","a","c"]', '[1,2,3,4,5,6,7,8,9,10]'],
  'hash'     => ['{}', '{a: 1}', '{"a"=>1,"b"=>2}', '{a: 1, b: 2, c: 3}', '{x: 10, y: 20}',
                 '{1=>2,3=>4}', '{a: [1,2], b: [3,4]}', '{"x"=>1}', '{k: "v"}',
                 '{one: 1, two: 2, three: 3, four: 4}'],
  'range'    => ['(1..5)', '(1...5)', '(0..0)', '("a".."e")', '(1..10)', '(-3..3)', '(10..20)',
                 '(0...10)', '(1..1)', '("aa".."ad")', '(-5..-1)', '(1..100)', '(2..2)'],
  'nil'      => ['nil'],
  'true'     => ['true'],
  'false'    => ['false'],
}.freeze

ARGS = ['0', '1', '2', '3', '-1', '5', '10', '0.5', '100', 'nil', 'true', 'false',
        ':a', ':b', '"x"', '"a"', '"-"', '"o"', '[1,2]', '[]', '{}', '(1..2)', '1..3'].freeze

# Block templates appended for block-hungry methods (covered by curated lists too).
BLOCKS = ['{ |x| x }', '{ |x| x.to_s }', '{ |x| -x rescue x }',
          '{ |a, b| [a, b] }', '{ |k, v| [k, v] }'].freeze

# Methods we never call (nondeterministic / IO / meta / interactive / dangerous).
DENY = /\A(object_id|__id__|hash|equal\?|itself|display|tap|then|yield_self|
  method|methods|public_methods|private_methods|protected_methods|singleton_methods|
  instance_variable.*|instance_variables|remove_instance_variable|
  send|public_send|__send__|extend|define_singleton_method|
  enum_for|to_enum|lazy|
  print|puts|pp|p|putc|write|syswrite|warn|gets|readline|readlines|
  freeze|taint|untaint|trust|untrust|tainted\?|untrusted\?|
  rand|srand|object|binding|caller|exit|exit!|abort|fork|system|exec|spawn|
  sleep|at_exit|trap|raise|fail|throw|catch|loop|redo|
  define_method|alias_method|remove_method|undef_method|
  instance_eval|instance_exec|class_eval|module_eval|eval|
  shuffle|shuffle!|sample|
  pretty_print.*|to_yaml|marshal.*|_dump)\z/x

def addr?(s) = s =~ /0x[0-9a-f]{3,}/i || s =~ /#<|:0x/

def certify(expr)
  out = out2 = nil
  Timeout.timeout(1) do
    out  = eval(expr).inspect # rubocop:disable Security/Eval -- trusted literals
    out2 = eval(expr).inspect # second eval: reject per-call nondeterminism (random)
  end
  return nil if out.nil? || out != out2 || out.length > 180 || addr?(out)
  expr
rescue Exception # skip anything that raises
  nil
end

def methods_for(klass)
  base = Object.public_instance_methods + Kernel.public_instance_methods
  (klass.public_instance_methods - base).sort
end

def klass_of(cat)
  { 'integer' => Integer, 'float' => Float, 'string' => String, 'symbol' => Symbol,
    'array' => Array, 'hash' => Hash, 'range' => Range, 'nil' => NilClass,
    'true' => TrueClass, 'false' => FalseClass }[cat]
end

lines = Hash.new { |h, k| h[k] = [] }

RECV.each do |cat, recvs|
  klass = klass_of(cat)
  meths = methods_for(klass) + %i[to_s inspect class size length frozen? nil? dup]
  meths.uniq!
  seen = {}
  recvs.each do |r|
    meths.each do |m|
      next if m.to_s =~ DENY
      pairs = ARGS.product(ARGS).select.with_index { |_, i| i % 9 == 0 }
      [[], *ARGS.map { |a| [a] }, *pairs].each do |argv|
        call = argv.empty? ? "(#{r}).#{m}" : "(#{r}).#{m}(#{argv.join(', ')})"
        next if seen[call]
        seen[call] = true
        e = certify(call)
        lines[cat] << "p #{e}" if e
      end
      # one block form for block-takers
      bcall = "(#{r}).#{m} #{BLOCKS[0]}"
      unless seen[bcall]
        seen[bcall] = true
        e = certify(bcall)
        lines[cat] << "p #{e}" if e
      end
    end
  end
end

# --- Phase 2: block-method matrix (Enumerable surface, near-zero from reflection) ---
ENUM_BLOCKS = [
  '.map { |x| x }', '.map { |x| x * 2 }', '.map { |x| [x] }', '.map(&:to_s)', '.map(&:class)',
  '.collect { |x| x.to_s }', '.flat_map { |x| [x, x] }', '.filter_map { |x| x if x.odd? }',
  '.select { |x| x.even? }', '.filter { |x| x != 1 }', '.reject { |x| x.even? }',
  '.find { |x| x > 2 }', '.detect { |x| x.odd? }', '.find_index { |x| x == 2 }',
  '.find_all { |x| x > 1 }', '.take_while { |x| x < 3 }', '.drop_while { |x| x < 3 }',
  '.reduce { |a, x| a + x }', '.reduce(0) { |a, x| a + x }', '.inject(:+)', '.inject(1, :*)',
  '.inject { |a, x| a * x }', '.sum { |x| x * 2 }', '.count { |x| x.odd? }',
  '.each_with_index.to_a', '.each_with_index.map { |x, i| [i, x] }', '.map.with_index { |x, i| i }',
  '.each_with_object([]) { |x, a| a << x }', '.group_by { |x| x.even? }',
  '.partition { |x| x.even? }', '.chunk_while { |a, b| b > a }.to_a', '.slice_when { |a, b| b < a }.to_a',
  '.sort_by { |x| -x }', '.min_by { |x| -x }', '.max_by { |x| -x }', '.minmax_by { |x| x }',
  '.sort { |a, b| b <=> a }', '.all? { |x| x > 0 }', '.any? { |x| x > 2 }', '.none? { |x| x > 9 }',
  '.one? { |x| x == 1 }', '.tally', '.minmax', '.each_slice(2).to_a', '.each_cons(2).to_a',
  '.flat_map { |x| x.to_s.chars }', '.chunk { |x| x.even? }.to_a',
].freeze
HASH_BLOCKS = [
  '.map { |k, v| [k, v] }', '.select { |k, v| true }', '.reject { |k, v| false }',
  '.filter_map { |k, v| k }', '.each_with_object([]) { |(k, v), a| a << k }',
  '.min_by { |k, v| v }', '.max_by { |k, v| v }', '.sort_by { |k, v| v }',
  '.transform_values { |v| v * 2 }', '.transform_keys(&:to_s)', '.count { |k, v| true }',
  '.find { |k, v| true }', '.sum { |k, v| v }', '.group_by { |k, v| v.even? }',
  '.partition { |k, v| v.even? }', '.flat_map { |k, v| [k, v] }', '.all? { |k, v| v > 0 }',
  '.any? { |k, v| v > 1 }', '.each_pair.to_a', '.reduce(0) { |a, (k, v)| a + v }',
].freeze
%w[array range].each do |cat|
  RECV[cat].each do |r|
    ENUM_BLOCKS.each { |t| (e = certify("(#{r})#{t}")) && (lines[cat] << "p #{e}") }
  end
end
RECV['hash'].each do |r|
  HASH_BLOCKS.each { |t| (e = certify("(#{r})#{t}")) && (lines['hash'] << "p #{e}") }
end

# --- Phase 3: Math module (deterministic float functions) ---
MATHFN = %w[sqrt cbrt sin cos tan asin acos atan sinh cosh tanh exp log log2 log10 erf erfc gamma lgamma frexp].freeze
NUMS   = %w[0.0 0.1 0.25 0.5 1.0 1.5 2.0 3.0 10.0 -1.0 -0.5 100.0].freeze
MATHFN.each do |f|
  NUMS.each { |n| (e = certify("Math.#{f}(#{n})")) && (lines['math'] << "p #{e}") }
end
%w[hypot atan2 ldexp].each do |f|
  NUMS.product(NUMS).each { |a, b| (e = certify("Math.#{f}(#{a}, #{b})")) && (lines['math'] << "p #{e}") }
end
['Math::PI', 'Math::E', 'Math.sqrt(2)', 'Math.log(Math::E)'].each do |c|
  (e = certify(c)) && (lines['math'] << "p #{e}")
end

require 'fileutils'
FileUtils.mkdir_p(OUT)
total = 0
files = 0
lines.each do |cat, ls|
  ls.uniq!
  ls.each_slice(CHUNK).with_index do |slice, i|
    path = File.join(OUT, format('%s_%03d.rb', cat, i))
    File.write(path, "# generated golden: #{cat} ##{i} (oracle = CRuby)\n" + slice.join("\n") + "\n")
    files += 1
    total += slice.size
  end
  warn format('  %-8s %5d assertions', cat, ls.size)
end
warn format('TOTAL %d assertions across %d files in %s/', total, files, OUT)
