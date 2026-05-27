require_relative "../../test_helper"

# Ruby 3.x syntax: endless method def, hash shorthand, numbered block params.

# ---------- endless method def ----------

class EndlessHost
  def add(a, b) = a + b
  def square(x) = x * x
  def hello = "hi"           # zero-arg
end

def test_endless_def_with_args
  assert_equal 5,  EndlessHost.new.add(2, 3)
  assert_equal 16, EndlessHost.new.square(4)
end

def test_endless_def_no_args
  assert_equal "hi", EndlessHost.new.hello
end

# ---------- hash shorthand `{x:, y:}` ----------

def test_hash_shorthand_basic
  x = 10
  y = 20
  result = {x:, y:}
  assert_equal({x: 10, y: 20}, result)
end

def test_hash_shorthand_mixed_with_explicit
  a = 1
  result = {a:, b: 2}
  assert_equal({a: 1, b: 2}, result)
end

def test_hash_shorthand_in_call
  def f(h); h; end
  v = "hello"
  assert_equal({v: "hello"}, f(v:))
end

# ---------- numbered block params ----------

def test_numbered_block_param_1
  assert_equal [2, 4, 6], [1, 2, 3].map { _1 * 2 }
end

def test_numbered_block_param_2
  pairs = [[1, 2], [3, 4]]
  result = pairs.map { _1 + _2 }
  assert_equal [3, 7], result
end

def test_numbered_block_inside_iter
  count = [0]
  [1, 2, 3, 4, 5].each { count[0] += _1 if _1.odd? }
  assert_equal 9, count[0]    # 1 + 3 + 5
end

TESTS = [
  :test_endless_def_with_args,
  :test_endless_def_no_args,
  :test_hash_shorthand_basic,
  :test_hash_shorthand_mixed_with_explicit,
  :test_hash_shorthand_in_call,
  :test_numbered_block_param_1,
  :test_numbered_block_param_2,
  :test_numbered_block_inside_iter,
]
TESTS.each { |t| run_test(t) }
report "Ruby3Syntax"
