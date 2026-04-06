require_relative 'test_helper'

# node_class_def
assert_eval "empty class", "class Foo; end; nil", nil

# class with method
assert_eval "class def + call",
  "class Foo; def bar; 42; end; end; Foo.new.bar", 42

# class with initialize
assert_eval "initialize no args",
  "class Foo; def initialize; @x = 99; end; def x; @x; end; end; Foo.new.x", 99

assert_eval "initialize 1 arg",
  "class Foo; def initialize(v); @v = v; end; def v; @v; end; end; Foo.new(42).v", 42

assert_eval "initialize 2 args",
  "class P; def initialize(x, y); @x = x; @y = y; end; " \
  "def sum; @x + @y; end; end; P.new(3, 4).sum", 7

# reopen class
assert_eval "reopen class",
  "class Foo; def a; 1; end; end; class Foo; def b; 2; end; end; Foo.new.a + Foo.new.b", 3

# multiple instances
assert_eval "multiple instances",
  "class C; def initialize(v); @v = v; end; def v; @v; end; end; " \
  "a = C.new(10); b = C.new(20); a.v + b.v", 30

# instance independence
assert_eval "instance independence",
  "class C; def initialize(v); @v = v; end; def v; @v; end; end; " \
  "a = C.new(1); b = C.new(2); c = C.new(3); a.v + b.v + c.v", 6

# ivar default is nil
assert_eval "ivar default nil",
  "class C; def x; @x; end; end; C.new.x", nil

# ivar mutation
assert_eval "ivar mutation",
  "class C; def initialize; @x = 0; end; def inc; @x += 1; end; def x; @x; end; end; " \
  "c = C.new; c.inc; c.inc; c.inc; c.x", 3

# method override
assert_eval "method override",
  "class Foo; def val; 1; end; end; a = Foo.new.val; " \
  "class Foo; def val; 2; end; end; a + Foo.new.val", 3

# inspect override
assert_eval "custom inspect",
  'class Foo; def inspect; "hello"; end; end; Foo.new.inspect', "hello"

# class method calling other methods on self
assert_eval "self method dispatch",
  "class Calc; def double(x); x * 2; end; def quad(x); self.double(self.double(x)); end; end; " \
  "Calc.new.quad(3)", 12

# operator defined on class
assert_eval "class operator",
  "class Vec; def initialize(x); @x = x; end; def +(other); Vec.new(@x + other.x); end; " \
  "def x; @x; end; end; " \
  "a = Vec.new(3); b = Vec.new(4); c = a + b; c.x", 7

# == on user object (inherits from Object)
assert_eval "user obj == identity",
  "class Foo; end; a = Foo.new; a == a", true
assert_eval "user obj == different",
  "class Foo; end; Foo.new == Foo.new", false

# != on user object
assert_eval "user obj !=",
  "class Foo; end; Foo.new != Foo.new", true

# nil? on user object
assert_eval "user obj nil?",
  "class Foo; end; Foo.new.nil?", false

# class name
assert_eval "user obj class",
  "class MyClass; end; MyClass.new.class", "MyClass"

# method_missing
assert_eval "method_missing basic",
  'class Ghost; def method_missing(n); "got:" + n; end; end; Ghost.new.anything', "got:anything"

# method_missing does not override existing
assert_eval "method_missing fallback",
  "class X; def foo; 1; end; def method_missing(n); 99; end; end; X.new.foo", 1
