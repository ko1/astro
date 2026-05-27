require_relative "../../test_helper"

# super / method dispatch edge cases.

class Base1
  def greet
    "hello"
  end
  def with_args(a, b)
    [a, b]
  end
end

class Sub1 < Base1
  def greet
    "[" + super + "]"
  end
  def with_args(a, b)
    super(a, b * 2)
  end
end

def test_super_no_args
  assert_equal "[hello]", Sub1.new.greet
end

def test_super_with_explicit_args
  assert_equal [1, 4], Sub1.new.with_args(1, 2)
end

# ---------- super calls forward args by default ----------

class Base2
  def call(*xs)
    xs.sum
  end
end

class Sub2 < Base2
  def call(*xs)
    super
  end
end

def test_super_forward_args
  assert_equal 6, Sub2.new.call(1, 2, 3)
end

# ---------- super through 3-level chain ----------

class Base3
  def name; "B"; end
end
class Mid3 < Base3
  def name; "M(" + super + ")"; end
end
class Top3 < Mid3
  def name; "T[" + super + "]"; end
end

def test_super_three_levels
  assert_equal "T[M(B)]", Top3.new.name
end

# ---------- module#prepend / super into prepended module ----------

module Greeter
  def hi
    "Greeter+" + super
  end
end

class Plain
  def hi; "plain"; end
end

class Wrapped < Plain
  prepend Greeter
end

def test_super_through_prepend
  assert_equal "Greeter+plain", Wrapped.new.hi
end

TESTS = [
  :test_super_no_args,
  :test_super_with_explicit_args,
  :test_super_forward_args,
  :test_super_three_levels,
  :test_super_through_prepend,
]
TESTS.each { |t| run_test(t) }
report "SuperDispatch"
