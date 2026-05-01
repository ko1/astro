require_relative "test_helper"

# Multi-assign
def test_masgn_basic
  a, b = 1, 2
  assert_equal 1, a
  assert_equal 2, b
end

def test_masgn_from_array
  a, b, c = [10, 20, 30]
  assert_equal 10, a
  assert_equal 20, b
  assert_equal 30, c
end

def test_masgn_short
  a, b, c = [1, 2]
  assert_equal 1, a
  assert_equal 2, b
  assert_equal nil, c
end

def test_masgn_swap
  a, b = 1, 2
  a, b = b, a
  assert_equal 2, a
  assert_equal 1, b
end

# Splat in method call
def test_splat_call
  def f(a, b, c)
    a + b + c
  end
  args = [10, 20, 30]
  assert_equal 60, f(*args)
end

# Rest parameter
def test_rest_param
  def g(a, *rest)
    [a, rest]
  end
  assert_equal [1, [2, 3, 4]], g(1, 2, 3, 4)
  assert_equal [1, []], g(1)
end

# Optional parameter
def test_optional
  def h(a, b = 10)
    a + b
  end
  assert_equal 13, h(3)
  assert_equal 7, h(3, 4)
end

# Send
def test_send
  class C
    def greet(name); "hi #{name}"; end
  end
  c = C.new
  assert_equal "hi alice", c.send(:greet, "alice")
  assert_equal "hi bob", c.send("greet", "bob")
end

# Method object
def test_method_obj
  class MM
    def add(a, b); a + b; end
  end
  m = MM.new.method(:add)
  assert_equal 7, m.call(3, 4)
  assert_equal 10, m[2, 8]
end

# instance_variable_set / get
def test_ivar_reflect
  class IVS
    def initialize
      @x = 10
    end
  end
  o = IVS.new
  assert_equal 10, o.instance_variable_get(:@x)
  o.instance_variable_set(:@y, 99)
  assert_equal 99, o.instance_variable_get(:@y)
end

# Symbol interpolation
def test_sym_interp
  i = 42
  assert_equal :"foo42", :"foo#{i}"
end

# nested method call destructuring etc.
def test_nested_call
  arr = [[1, 2], [3, 4]]
  out = arr.map {|a, b| a + b }
  assert_equal [3, 7], out
end

# block with extra params (less yielded args)
def test_block_extra_params
  def yield_one(x); yield x; end
  yield_one(7) {|a, b| assert_equal 7, a; assert_equal nil, b }
end

# Closure capture mutability (within yield)
def test_closure_yield
  counter = 0
  3.times { counter += 1 }
  assert_equal 3, counter
end

TESTS = %i[
  test_masgn_basic test_masgn_from_array test_masgn_short test_masgn_swap
  test_splat_call test_rest_param test_optional
  test_send test_method_obj test_ivar_reflect test_sym_interp
  test_nested_call test_block_extra_params test_closure_yield
]
TESTS.each {|t| run_test(t) }
report("Misc")
