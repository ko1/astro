require_relative 'test_helper'

# This file specifically tests GC safety under allocation pressure.
# Each test creates many objects to force GC cycles.

# === String GC pressure ===

assert_eval "gc: string concat loop", <<~'RUBY', 100
  i = 0
  while i < 100
    s = "hello" + " " + "world"
    i += 1
  end
  i
RUBY

assert_eval "gc: string interpolation loop", <<~'RUBY', 100
  i = 0
  while i < 100
    s = "value is #{i}"
    i += 1
  end
  i
RUBY

assert_eval "gc: string method chain", <<~'RUBY', 100
  i = 0
  while i < 100
    s = "hello".upcase.reverse.downcase
    i += 1
  end
  i
RUBY

assert_eval "gc: many string vars", <<~'RUBY', true
  a = "a"
  b = "b"
  c = "c"
  i = 0
  while i < 200
    x = a + b + c
    y = x.upcase
    z = y.reverse
    i += 1
  end
  z == "CBA"
RUBY

# === Object GC pressure ===

assert_eval "gc: many objects", <<~'RUBY', 1000
  class Obj; def initialize(v); @v = v; end; def v; @v; end; end
  i = 0
  while i < 1000
    o = Obj.new(i)
    i += 1
  end
  i
RUBY

assert_eval "gc: objects with string ivars", <<~'RUBY', 500
  class Named; def initialize(n); @name = n; end; def name; @name; end; end
  i = 0
  while i < 500
    o = Named.new("item")
    i += 1
  end
  i
RUBY

assert_eval "gc: object chain", <<~'RUBY', 99
  class Node
    def initialize(val, nxt)
      @val = val
      @nxt = nxt
    end
    def val; @val; end
    def nxt; @nxt; end
  end
  n = nil
  i = 0
  while i < 100
    n = Node.new(i, n)
    i += 1
  end
  n.val
RUBY

# === Array GC pressure ===

assert_eval "gc: array push loop", <<~'RUBY', 1000
  a = []
  i = 0
  while i < 1000
    a.push(i)
    i += 1
  end
  a.length
RUBY

assert_eval "gc: many temp arrays", <<~'RUBY', 500
  i = 0
  while i < 500
    a = [i, i + 1, i + 2, i + 3, i + 4]
    i += 1
  end
  i
RUBY

assert_eval "gc: array of strings", <<~'RUBY', 200
  a = []
  i = 0
  while i < 200
    a.push("item")
    i += 1
  end
  a.length
RUBY

assert_eval "gc: array of objects", <<~'RUBY', 100
  class Box; def initialize(v); @v = v; end; end
  a = []
  i = 0
  while i < 100
    a.push(Box.new(i))
    i += 1
  end
  a.length
RUBY

# === Hash GC pressure ===

assert_eval "gc: hash set loop", <<~'RUBY', 500
  h = {}
  i = 0
  while i < 500
    h[i] = i * 2
    i += 1
  end
  h.length
RUBY

assert_eval "gc: many temp hashes", <<~'RUBY', 500
  i = 0
  while i < 500
    h = {"a" => i, "b" => i + 1}
    i += 1
  end
  i
RUBY

assert_eval "gc: hash with string values", <<~'RUBY', 200
  h = {}
  i = 0
  while i < 200
    h[i] = "value"
    i += 1
  end
  h.length
RUBY

# === Mixed GC pressure ===

assert_eval "gc: mixed containers", <<~'RUBY', 100
  i = 0
  while i < 100
    a = [1, 2, 3]
    h = {"x" => a, "y" => "hello"}
    s = h["y"] + " world"
    i += 1
  end
  i
RUBY

assert_eval "gc: class with array and hash", <<~'RUBY', 50
  class Store
    def initialize
      @items = []
      @index = {}
    end
    def add(name, val)
      @items.push(val)
      @index[name] = val
    end
    def get(name)
      @index[name]
    end
    def count
      @items.length
    end
  end
  s = Store.new
  i = 0
  while i < 50
    s.add("k", i)
    i += 1
  end
  s.count
RUBY

assert_eval "gc: deep method calls with alloc", <<~'RUBY', 55
  def fib(n)
    if n < 2
      n
    else
      fib(n - 1) + fib(n - 2)
    end
  end
  fib(10)
RUBY

assert_eval "gc: fib with string result", <<~'RUBY', "55"
  def fib(n)
    if n < 2
      n
    else
      fib(n - 1) + fib(n - 2)
    end
  end
  fib(10).to_s
RUBY

assert_eval "gc: method_missing alloc", <<~'RUBY', 100
  class Sink
    def method_missing(name)
      name.length
    end
  end
  s = Sink.new
  i = 0
  total = 0
  while i < 100
    total = s.foo
    i += 1
  end
  i
RUBY

assert_eval "gc: inspect under pressure", <<~'RUBY', 100
  class Pt
    def initialize(x, y); @x = x; @y = y; end
    def inspect; "(#{@x}, #{@y})"; end
  end
  i = 0
  while i < 100
    pt = Pt.new(i, i + 1)
    s = pt.inspect
    i += 1
  end
  i
RUBY

assert_eval "gc: complex scenario", <<~'RUBY', 100
  class Person
    def initialize(name, age)
      @name = name
      @age = age
    end
    def inspect
      "Person(#{@name}, #{@age})"
    end
    def name; @name; end
    def age; @age; end
  end
  people = []
  i = 0
  while i < 100
    people.push(Person.new("user", i))
    i += 1
  end
  people.length
RUBY
