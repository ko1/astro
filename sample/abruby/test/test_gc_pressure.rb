require_relative 'test_helper'

class TestGCPressure < AbRubyTest
  def test_string_concat_loop = assert_eval(<<~'RUBY', 100)
    i = 0
    while i < 100
      s = "hello" + " " + "world"
      i += 1
    end
    i
  RUBY

  def test_string_interpolation_loop = assert_eval(<<~'RUBY', 100)
    i = 0
    while i < 100
      s = "value is #{i}"
      i += 1
    end
    i
  RUBY

  def test_string_method_chain = assert_eval(<<~'RUBY', 100)
    i = 0
    while i < 100
      s = "hello".upcase.reverse.downcase
      i += 1
    end
    i
  RUBY

  def test_many_string_vars = assert_eval(<<~'RUBY', true)
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

  def test_many_objects = assert_eval(<<~'RUBY', 1000)
    class TgObj; def initialize(v); @v = v; end; def v; @v; end; end
    i = 0
    while i < 1000
      o = TgObj.new(i)
      i += 1
    end
    i
  RUBY

  def test_objects_with_string_ivars = assert_eval(<<~'RUBY', 500)
    class TgNamed; def initialize(n); @name = n; end; def name; @name; end; end
    i = 0
    while i < 500
      o = TgNamed.new("item")
      i += 1
    end
    i
  RUBY

  def test_object_chain = assert_eval(<<~'RUBY', 99)
    class TgNode
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
      n = TgNode.new(i, n)
      i += 1
    end
    n.val
  RUBY

  def test_array_push_loop = assert_eval(<<~'RUBY', 1000)
    a = []
    i = 0
    while i < 1000
      a.push(i)
      i += 1
    end
    a.length
  RUBY

  def test_many_temp_arrays = assert_eval(<<~'RUBY', 500)
    i = 0
    while i < 500
      a = [i, i + 1, i + 2, i + 3, i + 4]
      i += 1
    end
    i
  RUBY

  def test_array_of_strings = assert_eval(<<~'RUBY', 200)
    a = []
    i = 0
    while i < 200
      a.push("item")
      i += 1
    end
    a.length
  RUBY

  def test_array_of_objects = assert_eval(<<~'RUBY', 100)
    class TgBox; def initialize(v); @v = v; end; end
    a = []
    i = 0
    while i < 100
      a.push(TgBox.new(i))
      i += 1
    end
    a.length
  RUBY

  def test_hash_set_loop = assert_eval(<<~'RUBY', 500)
    h = {}
    i = 0
    while i < 500
      h[i] = i * 2
      i += 1
    end
    h.length
  RUBY

  def test_many_temp_hashes = assert_eval(<<~'RUBY', 500)
    i = 0
    while i < 500
      h = {"a" => i, "b" => i + 1}
      i += 1
    end
    i
  RUBY

  def test_mixed_containers = assert_eval(<<~'RUBY', 100)
    i = 0
    while i < 100
      a = [1, 2, 3]
      h = {"x" => a, "y" => "hello"}
      s = h["y"] + " world"
      i += 1
    end
    i
  RUBY

  def test_class_with_containers = assert_eval(<<~'RUBY', 50)
    class TgStore
      def initialize
        @items = []
        @index = {}
      end
      def add(name, val)
        @items.push(val)
        @index[name] = val
      end
      def count
        @items.length
      end
    end
    s = TgStore.new
    i = 0
    while i < 50
      s.add("k", i)
      i += 1
    end
    s.count
  RUBY

  def test_fib = assert_eval(<<~'RUBY', 55)
    def fib(n)
      if n < 2
        n
      else
        fib(n - 1) + fib(n - 2)
      end
    end
    fib(10)
  RUBY

  def test_fib_string = assert_eval(<<~'RUBY', "55")
    def fib(n)
      if n < 2
        n
      else
        fib(n - 1) + fib(n - 2)
      end
    end
    fib(10).to_s
  RUBY

  def test_method_missing_alloc = assert_eval(<<~'RUBY', 100)
    class TgSink
      def method_missing(name)
        name.length
      end
    end
    s = TgSink.new
    i = 0
    while i < 100
      s.foo
      i += 1
    end
    i
  RUBY

  def test_inspect_under_pressure = assert_eval(<<~'RUBY', 100)
    class TgPt
      def initialize(x, y); @x = x; @y = y; end
      def inspect; "(#{@x}, #{@y})"; end
    end
    i = 0
    while i < 100
      pt = TgPt.new(i, i + 1)
      s = pt.inspect
      i += 1
    end
    i
  RUBY

  def test_complex_scenario = assert_eval(<<~'RUBY', 100)
    class TgPerson
      def initialize(name, age)
        @name = name
        @age = age
      end
      def inspect
        "TgPerson(#{@name}, #{@age})"
      end
    end
    people = []
    i = 0
    while i < 100
      people.push(TgPerson.new("user", i))
      i += 1
    end
    people.length
  RUBY
end
