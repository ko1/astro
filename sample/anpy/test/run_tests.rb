#!/usr/bin/env ruby
# frozen_string_literal: true
#
# Test harness for AnPy (ChocoPy).
#
# Valid programs are run through both `anpy` and `python3` and their stdout
# is compared byte-for-byte (ChocoPy programs are valid Python 3.6 with the
# same observable semantics — §A lists the few exceptions, which the cases
# below avoid: e.g. `print` is restricted to int/bool/str, and integer
# overflow is undefined).  Runtime-error programs abort in both, so the
# stdout printed *before* the error still matches.  Type-error programs
# must be rejected statically by anpy (python3 would run them).

require 'open3'

ANPY = File.expand_path('../anpy', __dir__)
PY   = ENV['PYTHON'] || 'python3'
abort "anpy not built (run make)" unless File.executable?(ANPY)

$pass = 0; $fail = 0; $failures = []

def out(cmd, input)
  o, _e, _s = Open3.capture3(*cmd, stdin_data: input)
  o
end

def diff(label, prog)
  a = out([ANPY], prog)
  b = out([PY], prog)
  if a == b then $pass += 1
  else $fail += 1; $failures << [label, prog, a, b] end
end

# anpy must reject (type error); python3 must accept (proving it is a
# *static* error, not a runtime/semantic one).
def reject(label, prog)
  a_out, a_err, a_st = Open3.capture3(ANPY, stdin_data: prog)
  ok = !a_st.success? && (a_out + a_err).include?('type error')
  if ok then $pass += 1
  else $fail += 1; $failures << ["reject:#{label}", prog, a_out + a_err, '(should be type error)'] end
end

# ---------------------------------------------------------------------
VALID = {
  'arith'        => "print(1 + 2 * 3 - 4)\nprint(7 // 2)\nprint(7 % 3)\nprint(-(2 + 3))",
  'floordiv_neg' => "print(-7 // 2)\nprint(-7 % 3)\nprint(7 // -2)\nprint(7 % -3)",
  'bool_logic'   => "print(True and False)\nprint(True or False)\nprint(not True)\nprint(1 < 2 and 2 < 3)",
  'compare'      => "print(1 < 2)\nprint(2 <= 2)\nprint(3 > 4)\nprint(5 == 5)\nprint(5 != 6)",
  'str_ops'      => "s:str = \"hello\"\nprint(len(s))\nprint(s[1])\nprint(s + \" world\")\nprint(s == \"hello\")",
  'str_index'    => "print(\"chocopy\"[3])\nprint(\"a\" + \"b\" + \"c\")",
  'ternary'      => "x:int = 5\nprint(1 if x > 0 else 2)\nprint(x if x < 0 else 0 - x)",
  'while_sum'    => "i:int = 1\ns:int = 0\nwhile i <= 100:\n    s = s + i\n    i = i + 1\nprint(s)",
  'multi_assign' => "a:int = 0\nb:int = 0\nc:int = 0\na = b = c = 9\nprint(a + b + c)",
  'global_var'   => "g:int = 10\ndef bump() -> int:\n    global g\n    g = g + 1\n    return g\nprint(bump())\nprint(bump())\nprint(g)",
  'func_rec'     => "def fib(n:int) -> int:\n    if n < 2:\n        return n\n    return fib(n-1) + fib(n-2)\nprint(fib(20))",
  'func_fact'    => "def fact(n:int) -> int:\n    if n < 2:\n        return 1\n    return n * fact(n-1)\nprint(fact(12))",
  'mutual_rec'   => "def isodd(n:int) -> bool:\n    if n == 0:\n        return False\n    return iseven(n-1)\ndef iseven(n:int) -> bool:\n    if n == 0:\n        return True\n    return isodd(n-1)\nprint(iseven(10))\nprint(isodd(7))",
  'nested_fn'    => "def outer(n:int) -> int:\n    x:int = 0\n    def inner() -> int:\n        nonlocal x\n        x = x + n\n        return x\n    return inner() + inner() + inner()\nprint(outer(5))",
  'closure_read' => "def make(n:int) -> int:\n    def use() -> int:\n        return n * 2\n    return use()\nprint(make(21))",
  'list_basic'   => "xs:[int] = None\nxs = [10, 20, 30, 40]\nxs[1] = 99\nprint(xs[0] + xs[1])\nprint(len(xs))\nprint(xs[3])",
  'list_concat'  => "a:[int] = None\nb:[int] = None\nc:[int] = None\na = [1, 2]\nb = [3, 4, 5]\nc = a + b\nprint(len(c))\nprint(c[4])",
  'list_loop'    => "xs:[int] = None\ne:int = 0\ns:int = 0\nxs = [3, 1, 4, 1, 5, 9, 2, 6]\nfor e in xs:\n    s = s + e\nprint(s)",
  'str_loop'     => "ch:str = \"\"\nn:int = 0\nfor ch in \"hello\":\n    n = n + 1\nprint(n)",
  'empty_list'   => "xs:[int] = None\nxs = []\nprint(len(xs))\nxs = [1]\nprint(len(xs))",
  'nested_list'  => "m:[[int]] = None\nrow:[int] = None\nm = [[1, 2], [3, 4]]\nrow = m[1]\nprint(row[0])\nprint(m[0][1])",

  # classes
  'class_basic'  => "class Point(object):\n    x:int = 0\n    y:int = 0\n    def sum(self:\"Point\") -> int:\n        return self.x + self.y\np:Point = None\np = Point()\np.x = 3\np.y = 4\nprint(p.sum())",
  'inherit'      => "class A(object):\n    def who(self:\"A\") -> str:\n        return \"A\"\nclass B(A):\n    def who(self:\"B\") -> str:\n        return \"B\"\na:A = None\na = B()\nprint(a.who())\na = A()\nprint(a.who())",
  'init'         => "class Counter(object):\n    n:int = 0\n    def __init__(self:\"Counter\"):\n        self.n = 100\n    def inc(self:\"Counter\") -> int:\n        self.n = self.n + 1\n        return self.n\nc:Counter = None\nc = Counter()\nprint(c.inc())\nprint(c.inc())",
  'inherit_attr' => "class Base(object):\n    tag:int = 7\n    def get(self:\"Base\") -> int:\n        return self.tag\nclass Derived(Base):\n    def __init__(self:\"Derived\"):\n        self.tag = 42\nd:Derived = None\nd = Derived()\nprint(d.get())",
  'tour_animal'  => "class animal(object):\n    makes_noise:bool = False\n    def make_noise(self:\"animal\") -> object:\n        if self.makes_noise:\n            print(self.sound())\n        return None\n    def sound(self:\"animal\") -> str:\n        return \"???\"\nclass cow(animal):\n    def __init__(self:\"cow\"):\n        self.makes_noise = True\n    def sound(self:\"cow\") -> str:\n        return \"moo\"\nc:animal = None\nc = cow()\nc.make_noise()",
  'is_op'        => "class Point(object):\n    x:int = 0\na:Point = None\nb:Point = None\nb = Point()\na = b\nprint(a is b)\nprint(a is None)\nb = Point()\nprint(a is b)",
  'linked_list'  => "class Node(object):\n    val:int = 0\n    next:\"Node\" = None\nn1:Node = None\nn2:Node = None\ncur:Node = None\ntotal:int = 0\nn1 = Node()\nn2 = Node()\nn1.val = 10\nn2.val = 20\nn1.next = n2\ncur = n1\nwhile not (cur is None):\n    total = total + cur.val\n    cur = cur.next\nprint(total)",
}
VALID.each { |n, p| diff(n, p) }

# Programs aborting at runtime (stdout prefix must still match python3).
RUNTIME = {
  'div_zero'  => "print(1)\nprint(10 // 0)",
  'mod_zero'  => "print(2)\nprint(10 % 0)",
  'idx_oob'   => "xs:[int] = None\nxs = [1, 2, 3]\nprint(xs[1])\nprint(xs[5])",
  'str_oob'   => "print(\"hi\"[0])\nprint(\"hi\"[9])",
  'none_attr' => "class C(object):\n    x:int = 0\nc:C = None\nprint(7)\nprint(c.x)",
}
RUNTIME.each { |n, p| diff("rt:#{n}", p) }

# Programs that must be rejected by the static type checker.
reject('assign_mismatch', "x:int = 0\nx = \"s\"")
reject('arith_bool',      "x:int = 0\nx = 1 + True")
reject('cond_not_bool',   "if 1 + 2:\n    pass")
reject('cmp_str',         "print(\"a\" < \"b\")")
reject('call_arity',      "def f(a:int, b:int) -> int:\n    return a + b\nprint(f(1))")
reject('call_argtype',    "def f(a:int) -> int:\n    return a\nprint(f(True))")
reject('ret_mismatch',    "def f() -> int:\n    return \"x\"")
reject('bad_attr',        "class C(object):\n    x:int = 0\nc:C = None\nc = C()\nprint(c.y)")
reject('index_nonint',    "xs:[int] = None\nxs = [1]\nprint(xs[\"a\"])")
reject('subclass_int',    "class C(int):\n    pass")
reject('and_nonbool',     "print(1 and 2)")
reject('undef_super',     "class C(Nope):\n    pass")

# Fixture programs.
Dir[File.join(__dir__, 'cases', '*.py')].sort.each { |f| diff("fixture:#{File.basename(f)}", File.read(f)) }

# ---------------------------------------------------------------------
puts
puts "passed: #{$pass}   failed: #{$fail}"
unless $failures.empty?
  puts "\n=== FAILURES ==="
  $failures.first(30).each do |label, prog, a, b|
    puts "- #{label}"
    puts "    anpy  : #{a.inspect}"
    puts "    python: #{b.inspect}"
  end
end
exit($fail.zero? ? 0 : 1)
