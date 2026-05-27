require_relative "../../test_helper"

# `p obj` and `[obj].inspect` should call user-defined #inspect when
# the class defines one (via korb_inspect_dispatch).  Previously
# nested inspect skipped user methods and fell through to "#<Cls:0x..>"

class Point
  def initialize(x, y) = (@x = x; @y = y)
  def inspect = "Pt(#{@x},#{@y})"
end

def test_p_calls_user_inspect
  pt = Point.new(3, 4)
  s = pt.inspect
  assert_equal "Pt(3,4)", s
end

def test_array_inspect_uses_user_inspect
  pt = Point.new(1, 2)
  assert_equal "[Pt(1,2)]", [pt].inspect
end

def test_hash_inspect_uses_user_inspect
  pt = Point.new(5, 6)
  result = { :a => pt }.inspect
  # user inspect for Pt is called; symbol key uses :a form.
  assert_equal "{:a=>Pt(5,6)}", result
end

def test_rational_inspect
  r = Rational(3, 4)
  assert_equal "(3/4)", r.inspect
end

def test_complex_inspect
  c = Complex(1, 2)
  assert_equal "(1+2i)", c.inspect
end

def test_array_of_rationals
  arr = [Rational(1, 2), Rational(2, 3)]
  s = arr.inspect
  assert_equal "[(1/2), (2/3)]", s
end

TESTS = [
  :test_p_calls_user_inspect,
  :test_array_inspect_uses_user_inspect,
  :test_hash_inspect_uses_user_inspect,
  :test_rational_inspect,
  :test_complex_inspect,
  :test_array_of_rationals,
]
TESTS.each { |t| run_test(t) }
report "UserInspect"
