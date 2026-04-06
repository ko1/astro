require_relative 'test_helper'

# === node_call (function call, no receiver) ===

# basic call
assert_eval "simple call", "def foo; 42; end; foo", 42

# call with 1 arg
assert_eval "call 1 arg", "def f(a); a; end; f(1)", 1

# call with 2 args
assert_eval "call 2 args", "def f(a, b); a + b; end; f(1, 2)", 3

# call with 3 args
assert_eval "call 3 args", "def f(a, b, c); a + b + c; end; f(1, 2, 3)", 6

# recursive call
assert_eval "recursion", "def fact(n); if n < 2; 1; else; n * fact(n - 1); end; end; fact(5)", 120
assert_eval "fib", "def fib(n); if n < 2; n; else; fib(n - 1) + fib(n - 2); end; end; fib(10)", 55

# nested calls: f(g(x))
assert_eval "nested call", "def f(a); a + 1; end; def g(a); a * 2; end; f(g(3))", 7

# call with expression args
assert_eval "expr arg", "def f(a); a; end; f(1 + 2)", 3

# call with call as arg
assert_eval "call as arg", "def f(a); a; end; def g(a); a * 10; end; f(g(5))", 50

# multiple calls in sequence
assert_eval "seq calls", "def f(a); a + 1; end; f(1); f(2); f(3)", 4

# call redefinition
assert_eval "redefine", "def f; 1; end; a = f; def f; 2; end; b = f; a + b * 10", 21

# mutual recursion
assert_eval "mutual recursion",
  "def even(n); if n == 0; true; else; odd(n - 1); end; end; " \
  "def odd(n); if n == 0; false; else; even(n - 1); end; end; " \
  "even(10)", true

# === node_method_call (receiver.method) ===

# method call on Integer
assert_eval "int method", "1 + 2", 3
assert_eval "int chain", "1 + 2 + 3", 6
assert_eval "int method no arg", "42.zero?", false
assert_eval "int method no arg 2", "0.zero?", true
assert_eval "int.abs", "(-5).abs", 5
assert_eval "int.to_s", "42.to_s", "42"
assert_eval "int.class", "1.class", "Integer"

# method call on String
assert_eval "str concat", '"a" + "b"', "ab"
assert_eval "str repeat", '"ab" * 3', "ababab"
assert_eval "str length", '"hello".length', 5
assert_eval "str upcase", '"hello".upcase', "HELLO"
assert_eval "str reverse", '"hello".reverse', "olleh"
assert_eval "str empty? false", '"x".empty?', false
assert_eval "str empty? true", '"".empty?', true
assert_eval "str.class", '"hello".class', "String"

# method call on bool/nil
assert_eval "true ==", "true == true", true
assert_eval "true !=", "true != false", true
assert_eval "nil.nil?", "nil.nil?", true
assert_eval "42.nil?", "42.nil?", false
assert_eval "nil.class", "nil.class", "NilClass"

# chained method calls
assert_eval "chain methods", '"hello".upcase.reverse', "OLLEH"
assert_eval "chain with args", '"ab".+("cd").length', 4

# method call result used as arg
assert_eval "method result as arg",
  "def f(a); a * 2; end; f(3 + 4)", 14

# method call result as receiver
assert_eval "method result as recv",
  "1.+(2).+(3)", 6

# === complex call patterns ===

# nested method calls: a.m(b.m(c))
assert_eval "nested method calls",
  "def f(a); a + 10; end; f(2 + 3)", 15

# binary op in call arg
assert_eval "binop in arg",
  "def f(a, b); a + b; end; f(1 + 2, 3 + 4)", 10

# call in method call arg
assert_eval "call in method arg",
  "def double(x); x * 2; end; 1 + double(3)", 7

# method call in call arg
assert_eval "method in call arg",
  "def f(a); a; end; f(42.to_s)", "42"

# deeply nested: f(g(h(x)))
assert_eval "deep nesting",
  "def f(x); x + 1; end; def g(x); x * 2; end; def h(x); x - 1; end; f(g(h(10)))", 19

# call with method_call arg: f(obj.method)
assert_eval "call with method_call arg",
  "def f(a); a + 1; end; f(5.abs)", 6

# multiple method calls as args
assert_eval "multi method args",
  "def f(a, b); a + b; end; f(3.abs, 4.abs)", 7

# method call inside while
assert_eval "method in while",
  "a = 0; i = 10; while i > 0; a += i; i -= 1; end; a", 55

# method call inside if
assert_eval "method in if",
  'a = "hello"; if a.length > 3; true; else; false; end', true

# === OOP: class method calls ===

# simple class method
assert_eval "class method",
  "class Foo; def bar; 42; end; end; Foo.new.bar", 42

# method with args
assert_eval "class method with args",
  "class Foo; def add(a, b); a + b; end; end; Foo.new.add(3, 4)", 7

# self in method
assert_eval "self identity",
  "class Foo; def me; self; end; end; f = Foo.new; f.me == f", true

# method calling another method on self
assert_eval "self method call",
  "class Foo; def a; 1; end; def b; self.a + 2; end; end; Foo.new.b", 3

# chained OOP calls
assert_eval "chained OOP",
  "class Foo; def val; 42; end; end; Foo.new.val.to_s", "42"

# method with ivar
assert_eval "ivar in method",
  "class Foo; def initialize(x); @x = x; end; def x; @x; end; end; Foo.new(10).x", 10

# method calling method on another object
assert_eval "method on other obj",
  "class Foo; def initialize(v); @v = v; end; def v; @v; end; " \
  "def add_v(other); @v + other.v; end; end; " \
  "a = Foo.new(3); b = Foo.new(4); a.add_v(b)", 7

# new with multiple args
assert_eval "new multi args",
  "class P; def initialize(x, y); @x = x; @y = y; end; " \
  "def sum; @x + @y; end; end; P.new(3, 4).sum", 7

# method_missing
assert_eval "method_missing",
  'class G; def method_missing(name); "got:" + name; end; end; G.new.hello', "got:hello"

assert_eval "method_missing with arg",
  "class G; def method_missing(name, x); x * 2; end; end; G.new.foo(21)", 42

# === edge cases ===

# empty method
assert_eval "empty method body", "def f; end; f", nil

# method returns expression
assert_eval "method returns expr",
  "def f(a, b); if a > b; a; else; b; end; end; f(3, 5)", 5

# overwrite method
assert_eval "overwrite method",
  "def f; 1; end; a = f; def f; 2; end; a + f", 3

# call with many args (4)
assert_eval "4 args",
  "def f(a, b, c, d); a + b + c + d; end; f(1, 2, 3, 4)", 10

# call with call results as multiple args
assert_eval "call results as args",
  "def double(x); x * 2; end; def add(a, b); a + b; end; add(double(3), double(4))", 14

# fib (stress test for call slots)
assert_eval "fib 15", "def fib(n); if n < 2; n; else; fib(n - 1) + fib(n - 2); end; end; fib(15)", 610

# nested class + method call
assert_eval "class in class method interaction",
  "class A; def val; 10; end; end; " \
  "class B; def val; 20; end; end; " \
  "A.new.val + B.new.val", 30

# method call with string interpolation result
assert_eval "interpolation as method result",
  'class Foo; def initialize(n); @n = n; end; def to_s; "Foo(#{@n})"; end; end; Foo.new(42).to_s',
  "Foo(42)"

# binary ops are method calls - verify dispatch
assert_eval "int + dispatch", "1.+(2)", 3
assert_eval "int - dispatch", "10.-(3)", 7
assert_eval "int * dispatch", "3.*(4)", 12
assert_eval "int < dispatch", "1.<(2)", true
assert_eval "int == dispatch", "3.==(3)", true

# string + dispatch
assert_eval 'str + dispatch', '"a".+("b")', "ab"
