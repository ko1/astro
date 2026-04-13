require_relative 'test_helper'

class TestFiber < AbRubyTest
  # Basic create + final return
  def test_fiber_basic = assert_eval(<<~RUBY, 42)
    f = Fiber.new { 42 }
    f.resume
  RUBY

  # Initial resume passes its arg as the block parameter
  def test_fiber_initial_arg = assert_eval(<<~RUBY, 11)
    f = Fiber.new { |x| x + 1 }
    f.resume(10)
  RUBY

  # One yield then return
  def test_fiber_one_yield = assert_eval(<<~RUBY, [11, 99])
    f = Fiber.new { |x| Fiber.yield(x + 1); 99 }
    [f.resume(10), f.resume]
  RUBY

  # The argument to the second resume becomes Fiber.yield's return value
  def test_fiber_resume_arg = assert_eval(<<~RUBY, [1, 7])
    f = Fiber.new do |x|
      y = Fiber.yield x
      [x, y]
    end
    f.resume(1)  # 1 (yielded)
    f.resume(7)  # [1, 7]
  RUBY

  # Multiple yield/resume pairs
  def test_fiber_many = assert_eval(<<~RUBY, [10, 20, 30, 99])
    f = Fiber.new do
      Fiber.yield 10
      Fiber.yield 20
      Fiber.yield 30
      99
    end
    [f.resume, f.resume, f.resume, f.resume]
  RUBY

  # Counter / generator
  def test_fiber_counter = assert_eval(<<~RUBY, [1, 2, 3, 4])
    counter = Fiber.new do
      n = 0
      loop do
        n += 1
        Fiber.yield n
      end
    end
    [counter.resume, counter.resume, counter.resume, counter.resume]
  RUBY

  # Fibonacci — verifies multi-assign with multiple RHS values evaluates
  # all RHSes BEFORE assigning (a, b = b, a+b would otherwise diverge).
  def test_fiber_fibonacci = assert_eval(<<~RUBY, [0, 1, 1, 2, 3, 5, 8])
    fib = Fiber.new do
      a, b = 0, 1
      loop do
        Fiber.yield a
        a, b = b, a + b
      end
    end
    out = []
    7.times { out << fib.resume }
    out
  RUBY

  # alive?
  def test_fiber_alive_running = assert_eval(<<~RUBY, true)
    f = Fiber.new { Fiber.yield 1 }
    f.resume
    f.alive?
  RUBY

  def test_fiber_alive_done = assert_eval(<<~RUBY, false)
    f = Fiber.new { 1 }
    f.resume
    f.alive?
  RUBY

  # Resuming a finished fiber raises
  def test_fiber_dead_raises = assert_eval(<<~RUBY, "rescued")
    f = Fiber.new { 1 }
    f.resume
    begin
      f.resume
      "no raise"
    rescue => e
      "rescued"
    end
  RUBY

  # Closure: fiber body sees enclosing locals
  def test_fiber_closure = assert_eval(<<~RUBY, [10, 20, 30])
    x = 10
    f = Fiber.new { Fiber.yield x; Fiber.yield x * 2; x * 3 }
    [f.resume, f.resume, f.resume]
  RUBY

  # Nested method call inside fiber body
  def test_fiber_calls_method = assert_eval(<<~RUBY, [11, 22])
    def double(x); x * 2; end
    f = Fiber.new do |a|
      Fiber.yield a + 1
      double(11)
    end
    [f.resume(10), f.resume]
  RUBY

  # Fiber inside a method (escapes the method's frame)
  def test_fiber_outlives_method = assert_eval(<<~RUBY, 42)
    def make
      Fiber.new { Fiber.yield 42; 100 }
    end
    f = make
    f.resume
  RUBY

  # Yield from a deeply nested call inside the fiber
  def test_fiber_yield_from_nested = assert_eval(<<~RUBY, [10, 20])
    def helper(n)
      Fiber.yield n
      n * 2
    end
    f = Fiber.new { x = helper(10); Fiber.yield x; x }
    [f.resume, f.resume]
  RUBY

  # Two independent fibers
  def test_two_fibers = assert_eval(<<~RUBY, [1, 100, 2, 200, 3, 300])
    a = Fiber.new { Fiber.yield 1; Fiber.yield 2; 3 }
    b = Fiber.new { Fiber.yield 100; Fiber.yield 200; 300 }
    [a.resume, b.resume, a.resume, b.resume, a.resume, b.resume]
  RUBY

  # The block parameter inside the fiber works
  def test_fiber_block_param = assert_eval(<<~RUBY, 100)
    Fiber.new { |a, b, c| a + b + c }.resume(50, 30, 20) rescue Fiber.new { |a| a }.resume(100)
  RUBY

  # GC: heap strings on root fiber survive GC triggered by allocations
  # inside a child fiber.
  def test_fiber_gc_root_stack = assert_eval(<<~RUBY, "root_ok")
    s = "root" + "_ok"
    f = Fiber.new do
      i = 0
      while i < 500
        x = "alloc_" + i.to_s
        i += 1
      end
      Fiber.yield 42
      99
    end
    f.resume
    s
  RUBY

  # GC: fiber wrapper stays alive across multiple resumes with GC
  # pressure (allocations) in between.
  def test_fiber_gc_wrapper_alive = assert_eval(<<~RUBY, [1, 2, 3])
    f = Fiber.new do
      Fiber.yield 1
      Fiber.yield 2
      3
    end
    out = []
    3.times do
      i = 0
      while i < 500
        x = "pressure_" + i.to_s
        i += 1
      end
      out << f.resume
    end
    out
  RUBY

  # GC: multiple suspended fibers survive GC pressure.
  def test_fiber_gc_multiple = assert_eval(<<~RUBY, [10, 100, 20, 200])
    a = Fiber.new { Fiber.yield 10; 20 }
    b = Fiber.new { Fiber.yield 100; 200 }
    r = []
    r << a.resume
    r << b.resume
    i = 0
    while i < 500
      x = "gc_" + i.to_s
      i += 1
    end
    r << a.resume
    r << b.resume
    r
  RUBY
end
