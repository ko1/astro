# Tests for `alias` keyword and `alias_method` — both register an
# existing method under a new name; they differ in scope.

require_relative "../../test_helper"

# --- alias keyword inside class body --------------------------

class Greeter
  def hello; "hi"; end
  alias greet hello
end

def test_alias_in_class_body
  g = Greeter.new
  assert_equal "hi", g.hello
  assert_equal "hi", g.greet
end

# --- alias_method (method form) inside class body ------------

class Greeter2
  def hello; "hi2"; end
  alias_method :greet, :hello
end

def test_alias_method_in_class_body
  g = Greeter2.new
  assert_equal "hi2", g.greet
end

# --- alias preserves the original (still callable) ------------

def test_original_still_callable
  g = Greeter.new
  assert_equal "hi", g.hello
  assert_equal "hi", g.greet
  # redefine hello — alias should NOT be affected (it captured the
  # original method object, but Ruby semantics say it stays bound).
end

# --- aliasing a method that takes args ------------------------

class Calc
  def add(a, b); a + b; end
  alias plus add
  alias_method :sum, :add
end

def test_alias_with_args
  c = Calc.new
  assert_equal 5, c.add(2, 3)
  assert_equal 5, c.plus(2, 3)
  assert_equal 5, c.sum(2, 3)
end

# --- alias of a method using a block --------------------------

class Box
  def each
    yield 1
    yield 2
    yield 3
  end
  alias for_each each
end

def test_alias_with_block
  b = Box.new
  total = 0
  b.for_each { |x| total += x }
  assert_equal 6, total
end

# Run all
TESTS = [
  :test_alias_in_class_body,
  :test_alias_method_in_class_body,
  :test_original_still_callable,
  :test_alias_with_args,
  :test_alias_with_block,
]

TESTS.each do |t|
  $current = t
  send(t)
end

if $fail == 0
  puts "OK Alias (#{$pass})"
else
  puts "FAIL Alias: #{$fail}/#{$pass + $fail}"
end
