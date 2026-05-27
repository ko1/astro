# Tests for korb_object_new ivar preallocation + ivar inline cache.
# Regression coverage for the recent ivar slot prealloc change.

require_relative "test_helper"

class Point
  def initialize(x, y)
    @x = x
    @y = y
  end
  def x = @x
  def y = @y
  def x=(v); @x = v; end
  def y=(v); @y = v; end
  def both; [@x, @y]; end
end

# --- basic ivar set + get ---------------------------------------

def test_basic_ivar
  p = Point.new(3, 4)
  assert_equal 3, p.x
  assert_equal 4, p.y
end

def test_ivar_after_assign
  p = Point.new(0, 0)
  p.x = 10
  p.y = 20
  assert_equal 10, p.x
  assert_equal 20, p.y
end

def test_many_objects_independent
  # Each object should have its own ivar slots — preallocation must
  # not share state across instances.
  a = Point.new(1, 2)
  b = Point.new(3, 4)
  c = Point.new(5, 6)
  assert_equal 1, a.x
  assert_equal 4, b.y
  assert_equal 5, c.x
end

def test_ivar_returned_in_array
  p = Point.new(7, 8)
  pair = p.both
  assert_equal 7, pair[0]
  assert_equal 8, pair[1]
end

# --- ivar set/get tight loop (matches bm_ivar pattern) ----------

class Counter
  def initialize; @count = 0; end
  def incr; @count += 1; end
  def count; @count; end
end

def test_counter_loop
  c = Counter.new
  100.times { c.incr }
  assert_equal 100, c.count
end

# --- ivar with mixed types (Float, String, nil, Array, ...) ----

class Bag
  def initialize
    @i = 1
    @f = 1.5
    @s = "hi"
    @n = nil
    @a = [1, 2]
  end
  def i = @i
  def f = @f
  def s = @s
  def n = @n
  def a = @a
end

def test_mixed_ivars
  b = Bag.new
  assert_equal 1, b.i
  assert_equal 1.5, b.f
  assert_equal "hi", b.s
  assert_equal nil, b.n
  assert_equal [1, 2], b.a
end

# --- subclass keeps parent ivars + adds own --------------------

class Animal
  def initialize(name); @name = name; end
  def name = @name
end

class Dog < Animal
  def initialize(name, breed)
    @name = name
    @breed = breed
  end
  def breed = @breed
end

def test_subclass_ivars
  d = Dog.new("Rex", "Lab")
  assert_equal "Rex", d.name
  assert_equal "Lab", d.breed
end

# --- ivar reading nil for unset slot (only matters when class
#     has the slot but a particular instance hasn't written it) ---

class Sometimes
  def initialize(set_one)
    if set_one
      @x = 1
    end
  end
  def x = @x
end

def test_ivar_unset_returns_nil
  yes = Sometimes.new(true)
  no  = Sometimes.new(false)
  assert_equal 1, yes.x
  assert_equal nil, no.x
end

# Run all
TESTS = [
  :test_basic_ivar, :test_ivar_after_assign,
  :test_many_objects_independent, :test_ivar_returned_in_array,
  :test_counter_loop, :test_mixed_ivars, :test_subclass_ivars,
  :test_ivar_unset_returns_nil,
]

TESTS.each do |t|
  $current = t
  send(t)
end

if $fail == 0
  puts "OK ObjectAlloc (#{$pass})"
else
  puts "FAIL ObjectAlloc: #{$fail}/#{$pass + $fail}"
end
