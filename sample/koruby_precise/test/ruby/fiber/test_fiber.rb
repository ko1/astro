require_relative "../../test_helper"

def test_basic
  f = Fiber.new do
    Fiber.yield 1
    Fiber.yield 2
    3
  end
  assert_equal 1, f.resume
  assert_equal 2, f.resume
  assert_equal 3, f.resume
end

def test_resume_with_args
  f = Fiber.new do |a|
    b = Fiber.yield(a + 1)
    c = Fiber.yield(b + 1)
    c + 1
  end
  assert_equal 11, f.resume(10)
  assert_equal 21, f.resume(20)
  assert_equal 31, f.resume(30)
end

def test_state
  f = Fiber.new { :done }
  assert_equal :done, f.resume
  # second resume on dead should raise
  raised = false
  begin
    f.resume
  rescue
    raised = true
  end
  assert_equal true, raised
end

class FiberHelper
  def dump(tag); Fiber.yield(tag); end
  def tick(n); n.times { Fiber.yield :t }; end
end

# Yield from a method called from inside the fiber body — exercises that
# the fiber's c->fp/sp are restored to the inner method's frame on resume.
def test_yield_from_method
  results = []
  helper = FiberHelper.new
  f = Fiber.new do
    helper.dump(:a)
    helper.dump(:b)
    :done
  end
  results << f.resume
  results << f.resume
  results << f.resume
  assert_equal [:a, :b, :done], results
end

# Yields from a sequence of method calls — used to trigger a "stack overflow"
# because the value-stack-end check ran against the resumer's stack while
# the fiber was using its own heap frame.
def test_many_method_yields
  out = []
  obj = FiberHelper.new
  f = Fiber.new do
    obj.tick(5)
    :end
  end
  6.times { out << f.resume }
  assert_equal [:t, :t, :t, :t, :t, :end], out
end

# An infinite-loop fiber yielding repeatedly — tests that c->sp inside the
# fiber doesn't accumulate beyond the fiber's heap frame.
def test_infinite_loop_fiber
  f = Fiber.new do
    i = 0
    while true
      Fiber.yield(i)
      i += 1
    end
  end
  vals = []
  100.times { vals << f.resume }
  assert_equal (0..99).to_a, vals
end

# Yield true vs nil distinction (PPU.run uses this to know frame boundaries).
def test_yield_returning_truthy
  f = Fiber.new do
    Fiber.yield true
    Fiber.yield nil
    Fiber.yield false
    :end
  end
  assert_equal true, f.resume
  assert_equal nil, f.resume
  assert_equal false, f.resume
  assert_equal :end, f.resume
end

# After the fiber dies, a second resume should raise.
def test_dead_fiber_raises
  f = Fiber.new { :done }
  f.resume
  raised = false
  begin
    f.resume
  rescue => _
    raised = true
  end
  assert_equal true, raised
end

# Resume passes args back into yield.  Multiple round trips.
def test_round_trip_args
  log = []
  f = Fiber.new do |first|
    log << first
    a = Fiber.yield(:y1)
    log << a
    b = Fiber.yield(:y2)
    log << b
    :end
  end
  assert_equal :y1, f.resume(10)
  assert_equal :y2, f.resume(20)
  assert_equal :end, f.resume(30)
  assert_equal [10, 20, 30], log
end

# Two independent fibers don't share state.
def test_two_independent_fibers
  f1 = Fiber.new do
    Fiber.yield 1
    Fiber.yield 2
  end
  f2 = Fiber.new do
    Fiber.yield 100
    Fiber.yield 200
  end
  assert_equal 1, f1.resume
  assert_equal 100, f2.resume
  assert_equal 2, f1.resume
  assert_equal 200, f2.resume
end

class Counter
  def initialize; @count = 0; end
  def step; @count += 1; Fiber.yield @count; end
  def total; @count; end
end

# Fiber that calls into a class with ivars — verifies the fiber's value-stack
# substitution doesn't mangle the receiver's ivars.
def test_fiber_with_ivars
  obj = Counter.new
  f = Fiber.new { 5.times { obj.step } }
  vals = []
  5.times { vals << f.resume }
  assert_equal [1, 2, 3, 4, 5], vals
  assert_equal 5, obj.total
end

TESTS = %i[
  test_basic test_resume_with_args test_state
  test_yield_from_method test_many_method_yields
  test_infinite_loop_fiber test_yield_returning_truthy
  test_dead_fiber_raises test_round_trip_args
  test_two_independent_fibers test_fiber_with_ivars
]
TESTS.each {|t| run_test(t) }
report("Fiber")
