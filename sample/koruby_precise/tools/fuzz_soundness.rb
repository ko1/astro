# Soundness fuzzer: generate diverse Ruby snippets, run each through koruby and
# real ruby, and flag crashes (SIGSEGV) or output divergence.  Focused on the
# code shapes that surfaced slot/frame bugs: multiple assignment, array
# literals, operators with user receivers, rescue/ensure with variable
# matchers, closures capturing locals, and combinations nested in blocks.
#
# Usage: ruby tools/fuzz_soundness.rb [limit]
require 'open3'
HERE = File.expand_path('..', __dir__)
BIN  = "#{HERE}/koruby_precise"
TMP  = ENV['TMPDIR'] || '/tmp'

snips = []

# --- building blocks -------------------------------------------------------
# A snippet = [name, setup(top-level), body(wrapped in a context)].
BINOPS   = %w[+ - * / % ** & | ^ << >> == != < > <= >= <=>]
CTX = {
  'top'    => ->(code) { code },
  'method' => ->(code) { "def __m\n#{code}\nend\n__m" },
  'block'  => ->(code) { "[1].each do\n#{code}\nend" },
  'lambda' => ->(code) { "l = -> {\n#{code}\n}; l.call" },
  'nested' => ->(code) { "[1].each { [2].each { -> {\n#{code}\n}.call } }" },
}

def defop(op) # a class (top-level) defining a user operator that echoes its arg
  "class K; def #{op}(x); \"#{op}\#{x}\"; end; def [](x); \"[]\#{x}\"; end; end"
end

def add(snips, name, setup, body)
  CTX.each { |cname, wrap| snips << ["#{name}_#{cname}", "#{setup}\n#{wrap.call(body)}"] }
end

# 1) every binop as a value in a 2-/3-target massign and an array literal, in
#    every context — the node_shl class of bug.
BINOPS.each do |op|
  add(snips, "massign_#{op}",  defop(op), "k=K.new\na,b = k #{op} 1, k #{op} 2\np [a,b]")
  add(snips, "arraylit_#{op}", defop(op), "k=K.new\np [k #{op} 1, k #{op} 2, k #{op} 3]")
end

# 2) index [] / []= in massign / array RHS / as targets, per context
add(snips, "idx_massign",    defop('+'), "k=K.new\na,b = k[1], k[2]\np [a,b]")
add(snips, "idxset_massign", "",         "h={}; a,b = (h[:x]=1), (h[:y]=2)\np [h,a,b]")
add(snips, "idx_target",     "",         "h={}; h[:a], h[:b] = 1, 2\np h")

# 3) rescue with a variable / ivar / cvar / method / const / non-module matcher.
matchers = {
  'localvar'  => ["m = ArgumentError", "m"],
  'twovars'   => ["m1 = TypeError; m2 = ArgumentError", "m1, m2"],
  'method'    => ["def mm; ArgumentError; end", "mm"],
  'const'     => ["", "ArgumentError"],
  'nonmod'    => ["m = 42", "m"],
  'ternary'   => ["c = true", "(c ? ArgumentError : TypeError)"],
  'arrayidx'  => ["ms = [ArgumentError]", "ms[0]"],
}
matchers.each do |mname, (setup, expr)|
  add(snips, "rescue_#{mname}", setup,
      "begin\n  raise ArgumentError, \"boom\"\nrescue #{expr} => e\n  p [:caught, e.class, e.message]\nrescue => e2\n  p [:outer, e2.class]\nend")
end

# 4) ensure body reading captured / local vars
add(snips, "ensure_capture", "",
    "x = 7\nbegin\n  y = 3\nensure\n  p [x, (defined?(y) ? y : :nil)]\nend")
add(snips, "ensure_after_raise", "",
    "log=[]\nbegin\n  begin; raise \"e\"; ensure; log << :ens; end\nrescue => e; log << e.message; end\np log")

# 5) closures: capture + mutate outer locals across assignment forms
add(snips, "closure_massign", "", "a=0; b=0\nf = -> { a, b = a+1, b+2 }\nf.call; f.call\np [a,b]")
add(snips, "closure_opassign", "", "s = +\"\"\n3.times { s << \"x\" }\np s")

# 6) massign shapes: splat, nested, swap, star-capture
add(snips, "massign_swap", "", "a,b = 1,2\na,b = b,a\np [a,b]")
add(snips, "massign_splat", "", "a,*b,c = 1,2,3,4,5\np [a,b,c]")
add(snips, "massign_nested", "", "(a,b),c = [1,2],3\np [a,b,c]")
add(snips, "massign_star_rhs", "", "x=[10,20]; a,b,c = 0,*x\np [a,b,c]")

# 7) exception control flow: retry, throw/catch
add(snips, "retry", "", "n=0\nbegin\n  n+=1\n  raise \"e\" if n<3\nrescue\n  retry if n<3\nend\np n")
add(snips, "throwcatch", "", "r = catch(:x) { 5.times { |i| throw :x, i if i==2 }; :none }\np r")

# 8) operator-assign on ivar/index/or-assign
add(snips, "opasgn_ivar", "", "@a=1; @a += 2; p @a")
add(snips, "opasgn_index", "", "h=Hash.new(0); h[:k] += 5; h[:k] += 5; p h[:k]")
add(snips, "opasgn_orassign", "", "h={}; h[:k] ||= []; h[:k] << 1; h[:k] << 2; p h")

# 9) send with splat/kwargs/block in massign RHS, and yield
add(snips, "send_splat_massign", "def g(*a); a.sum; end",
    "x=[1,2]; a,b = g(*x), g(3,4)\np [a,b]")
add(snips, "yield_massign", "def h; a,b = yield(1), yield(2); [a,b]; end",
    "p(h { |v| v*10 })")

# 10) string/array building loops (GC-heavy) with user ops
add(snips, "build_str", "", "s = +\"\"\n100.times { |i| s << i.to_s << \",\" }\np s.length")
add(snips, "build_ary", "", "a = []\n100.times { |i| a << [i, i*i] }\np a.length")

# 11) unary operators (-@ +@ ~ !) with user classes
%w[-@ +@ ~].each do |uop|
  disp = uop.sub('@','')
  add(snips, "unary_#{uop}", "class U; def #{uop}; \"#{uop}\"; end; end",
      "u=U.new\np [#{disp}u, #{disp}u]")
end

# 12) keyword args, splat, double-splat, block-pass in various spots
add(snips, "kwargs", "def kw(a:, b: 9); [a,b]; end", "p [kw(a:1), kw(a:2,b:3)]")
add(snips, "double_splat", "def ds(**h); h; end", "o={x:1}; p ds(**o, y:2)")
add(snips, "splat_call", "def sp(*a, **k); [a,k]; end", "x=[1,2]; p sp(*x, k:3)")
add(snips, "block_pass", "def bp; yield 5; end", "f = ->(v){v*2}; p bp(&f)")
add(snips, "amp_symbol", "", "p [1,2,3].map(&:to_s)")

# 13) pattern matching (case/in)
add(snips, "pattern_array", "", "case [1,2,3]\nin [a, *rest]\n  p [a, rest]\nend")
add(snips, "pattern_hash", "", "case {name: \"x\", age: 3}\nin {name: String => n, age: Integer => a}\n  p [n, a]\nend")
add(snips, "pattern_find", "", "case [1,2,3,4]\nin [*, 2 => x, *]\n  p x\nend")
add(snips, "pattern_guard", "", "case 5\nin n if n > 3\n  p :big\nelse\n  p :small\nend")

# 14) numeric coercion & mixed arithmetic
add(snips, "coerce_user", "class M; def coerce(o); [o, 10]; end; def +(o); \"m+\#{o}\"; end; end",
    "m=M.new\np [1 + m, m + 1]")
add(snips, "bignum_ops", "", "b = 2**70\np [b + 1, b * 2, b % 7, b - (2**70), -b]")
add(snips, "rational_complex", "", "p [Rational(1,3) + Rational(1,6), Complex(1,2) * Complex(3,4)]")
add(snips, "float_int_mix", "", "p [1 + 2.0, 3.0 / 2, 7 % 2.5, 2 ** 0.5 > 1]")

# 15) super / inheritance / method resolution
add(snips, "super_chain", "class A; def g(x); \"A\#{x}\"; end; end\nclass B < A; def g(x); \"B\" + super; end; end",
    "p B.new.g(1)")
add(snips, "super_implicit_args", "class A; def h(a,b); [a,b]; end; end\nclass B < A; def h(a,b); super; end; end",
    "p B.new.h(1,2)")
add(snips, "module_include", "module M; def hi; \"M\#{who}\"; end; end\nclass C; include M; def who; \"C\"; end; end",
    "p C.new.hi")
add(snips, "prepend", "module P; def f; \"P\"+super; end; end\nclass C; prepend P; def f; \"C\"; end; end",
    "p C.new.f")

# 16) method_missing / respond_to_missing / define_method
add(snips, "method_missing", "class G; def method_missing(m, *a); \"mm:\#{m}:\#{a}\"; end; end",
    "p G.new.anything(1,2)")
add(snips, "define_method", "class D; define_method(:dm) { |x| x * 3 }; end", "p D.new.dm(4)")
add(snips, "send_variants", "class S; def foo(x); x; end; private def bar; :b; end; end",
    "s=S.new\np [s.send(:foo, 7), s.send(:bar), s.public_send(:foo, 8)]")

# 17) frozen / dup / clone / equality semantics
add(snips, "frozen", "", "s = \"x\".freeze\np [s.frozen?, s.dup.frozen?, s.clone.frozen?]")
add(snips, "hash_eql", "class K2; def hash; 42; end; def eql?(o); o.is_a?(K2); end; end",
    "h = {}; h[K2.new] = 1; p h[K2.new]")

# 18) Struct / Data
add(snips, "struct", "Pt = Struct.new(:x, :y) { def dist; x + y; end }",
    "p1 = Pt.new(3, 4); p [p1.x, p1.dist, p1.to_a, p1 == Pt.new(3,4)]")
add(snips, "data_define", "M2 = Data.define(:a, :b)", "m = M2.new(a: 1, b: 2); p [m.a, m.b, m.with(a: 9).a]")

# 19) Comparable / Enumerable mixins
add(snips, "comparable", "class Ver; include Comparable; attr_reader :n; def initialize(n); @n=n; end; def <=>(o); n <=> o.n; end; end",
    "p [Ver.new(1) < Ver.new(2), [Ver.new(3),Ver.new(1)].min.n]")
add(snips, "enumerable", "class Bag; include Enumerable; def initialize(*a); @a=a; end; def each(&b); @a.each(&b); end; end",
    "p [Bag.new(3,1,2).sort, Bag.new(1,2,3).map { |x| x*2 }, Bag.new(1,2,3).select(&:odd?)]")

# 20) nested exception control: return/break/next through ensure, retry counters
add(snips, "return_via_ensure", "def r; begin; return 1; ensure; nil; end; end", "p r")
add(snips, "break_from_block", "", "r = [1,2,3].each { |x| break x*10 if x == 2 }; p r")
add(snips, "next_value", "", "p [1,2,3].map { |x| next 0 if x == 2; x }")
add(snips, "ensure_return_override", "def r; begin; 1; ensure; return 2; end; end", "p r")
add(snips, "nested_rescue_reraise", "",
    "begin\n  begin\n    raise \"inner\"\n  rescue => e\n    raise TypeError, \"wrapped: \#{e.message}\"\n  end\nrescue => e2\n  p [e2.class, e2.message]\nend")

# 21) closures capturing loop variables / shared state
add(snips, "closure_counter", "", "def mk; c = 0; -> { c += 1 }; end\nf = mk\np [f.call, f.call, f.call]")
add(snips, "closures_share", "", "a = []; 3.times { |i| a << -> { i } }; p a.map(&:call)")
add(snips, "curry", "", "add = ->(a, b, c) { a + b + c }; p add.curry[1][2][3]")

# 22) randomly-generated deep expression trees (deterministic per index — no
#     Math.random; mixes operators, massign, blocks, rescue, ternary, index).
class Gen
  def initialize(seed); @s = seed; end
  def rnd(n); @s = (@s * 1103515245 + 12345) & 0x7fffffff; @s % n; end
  ATOMS = ['1', '2', '0', '-3', '10', 'x', 'y', '"s"', ':k', '[1,2]', 'nil', 'true']
  OPS   = %w[+ - * % & | ^ << == < > <=>]
  def expr(depth)
    return ATOMS[rnd(ATOMS.size)] if depth <= 0 || rnd(3) == 0
    case rnd(6)
    when 0 then "(#{expr(depth-1)} #{OPS[rnd(OPS.size)]} #{expr(depth-1)})"
    when 1 then "[#{expr(depth-1)}, #{expr(depth-1)}][#{rnd(2)}]"
    when 2 then "(#{expr(depth-1)}).to_s"
    when 3 then "(#{expr(depth-1)} ? #{expr(depth-1)} : #{expr(depth-1)})"
    when 4 then "[#{expr(depth-1)}, #{expr(depth-1)}].map { |z| z.to_s }"
    else "begin; #{expr(depth-1)}; rescue => e; #{expr(depth-1)}; end"
    end
  end
  def stmt(depth)
    case rnd(4)
    when 0 then "a, b = #{expr(depth)}, #{expr(depth)}; p [a, b]"
    when 1 then "x = #{rnd(5)}; y = #{rnd(5)}; p(#{expr(depth)})"
    when 2 then "arr = [#{expr(depth)}, #{expr(depth)}, #{expr(depth)}]; p arr"
    else "x = #{rnd(9)}; y = #{rnd(9)}; r = -> { #{expr(depth)} }; p r.call"
    end
  end
end
40.times do |i|
  g = Gen.new(i * 2654435761 + 1)
  add(snips, "rand#{i}", "", g.stmt(4))
end

# --- run each snippet through koruby and ruby ------------------------------
def run(bin, path, env = {})
  out, st = Open3.capture2e(env, bin, path)
  [out.force_encoding('UTF-8').scrub, st.exitstatus]   # tolerate binary output (marshal/pack)
rescue => e
  ["<runner-error: #{e.class}>", -1]
end
CRASH = ->(x) { [139, 134, 136, 132].include?(x) }

numarg = ARGV.find { |a| a =~ /\A\d+\z/ }
limit = numarg ? numarg.to_i : snips.size
stress = ARGV.include?('--stress')
snips = snips.first(limit)
crashes = []; diffs = []; stress_crashes = []; ok = 0
snips.each do |(name, code)|
  path = "#{TMP}/fuzz_#{Process.pid}.rb"
  File.write(path, code + "\n")
  ko, kx = run(BIN, path)
  if CRASH.(kx)
    crashes << [name, kx, code]
    next
  end
  # GC-stress pass: a moving-GC safety bug can crash only when GC runs often.
  if stress
    _so, sx = run(BIN, path, 'BARUBY_GC_STRESS' => '1', 'BARUBY_GC_PURGE' => '1')
    stress_crashes << [name, sx, code] if CRASH.(sx)
  end
  rb, _ = run('ruby', path)
  # Normalize: collapse an error to "ERR:<Class>:<message>" (ruby's newer verbose
  # format adds a source snippet + caret which koruby doesn't), strip addresses.
  norm = lambda do |s|
    s = s.gsub(/0x[0-9a-f]+/, '0xADDR').gsub(/#<(\w+):0xADDR>/, '#<\1>')
    if s =~ /\(([A-Z]\w*(?:Error|Exception|Interrupt))\)\s*\z/m ||
       (s =~ /:\s*(.+?)\s*\(([A-Z]\w*(?:Error|Exception))\)/m)
      # extract "message (Class)" from the first error line
      line = s.lines.find { |l| l =~ /\([A-Z]\w*(?:Error|Exception)\)/ } || s
      if line =~ /:\s*(.*?)\s*\(([A-Z]\w*(?:Error|Exception))\)/
        # NameError vs NoMethodError for a bare vcall is a known minor divergence;
        # treat them as equal so it doesn't drown real diffs.
        cls = $2.sub('NoMethodError', 'NameError')
        return "ERR:#{cls}:#{$1.split("\n").first}"
      end
    end
    s
  end
  if norm.call(ko) != norm.call(rb)
    diffs << [name, ko, rb, code]
  else
    ok += 1
  end
end

puts "=== fuzz_soundness: #{snips.size} snippets — ok=#{ok} diffs=#{diffs.size} crashes=#{crashes.size} stress_crashes=#{stress_crashes.size} ==="
unless stress_crashes.empty?
  puts "\n### STRESS-ONLY CRASHES (#{stress_crashes.size}) ###"
  stress_crashes.each { |n, x, c| puts "  SCRASH(#{x}) #{n}\n#{c.gsub(/^/, '    ')}" }
end
unless crashes.empty?
  puts "\n### CRASHES (#{crashes.size}) ###"
  crashes.each { |n, x, c| puts "  CRASH(#{x}) #{n}" }
end
unless diffs.empty?
  puts "\n### DIFFS (#{diffs.size}) ###"
  diffs.first(40).each do |n, ko, rb, _c|
    puts "  DIFF #{n}\n    koruby: #{ko.strip.inspect}\n    ruby  : #{rb.strip.inspect}"
  end
end
