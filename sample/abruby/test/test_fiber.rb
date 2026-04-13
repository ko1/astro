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

  # GC stress: fiber yields from inside a block passed to a method.
  def test_fiber_gc_yield_in_block = assert_eval(<<~RUBY, [1, 2, 3, 4, 5])
    def each_upto(n)
      i = 0
      while i < n
        yield i + 1
        i += 1
      end
    end
    f = Fiber.new do
      each_upto(5) do |v|
        i = 0
        while i < 200
          x = "blk_" + i.to_s
          i += 1
        end
        Fiber.yield v
      end
      999
    end
    out = []
    5.times { out << f.resume }
    out
  RUBY

  # GC stress: deep method calls inside fiber body with heavy allocation.
  def test_fiber_gc_deep_calls = assert_eval(<<~RUBY, 110)
    def fib(n)
      if n < 2
        n
      else
        fib(n - 1) + fib(n - 2)
      end
    end
    f = Fiber.new do
      Fiber.yield fib(10)
      fib(10)
    end
    r1 = f.resume
    i = 0
    while i < 500
      x = "stress_" + i.to_s
      i += 1
    end
    r2 = f.resume
    r1 + r2
  RUBY

  # GC stress: many short-lived fibers created and abandoned in a loop.
  def test_fiber_gc_many_abandoned = assert_eval(<<~RUBY, 50)
    count = 0
    50.times do |n|
      f = Fiber.new do
        s = "fiber_" + n.to_s
        Fiber.yield s.length
        999
      end
      count += 1 if f.resume > 0
      # f is abandoned (SUSPENDED) here — GC must handle it
      i = 0
      while i < 100
        x = "abandon_" + i.to_s
        i += 1
      end
    end
    count
  RUBY

  # GC stress: fiber creates heap objects that outlive the fiber.
  def test_fiber_gc_escaped_objects = assert_eval(<<~RUBY, 10)
    results = []
    10.times do |n|
      f = Fiber.new do
        obj = "obj_" + n.to_s
        Fiber.yield obj
      end
      results << f.resume
      i = 0
      while i < 200
        x = "esc_" + i.to_s
        i += 1
      end
    end
    results.length
  RUBY

  # GC stress: Proc captured inside fiber, called with allocation pressure.
  def test_fiber_gc_proc_in_fiber = assert_eval(<<~RUBY, [2, 4, 6])
    f = Fiber.new do
      mul = 2
      pr = Proc.new { |x| x * mul }
      Fiber.yield pr.call(1)
      Fiber.yield pr.call(2)
      pr.call(3)
    end
    out = []
    3.times do
      i = 0
      while i < 300
        x = "proc_" + i.to_s
        i += 1
      end
      out << f.resume
    end
    out
  RUBY

  # --- Stress tests below ---

  # Nested fibers: fiber creates and uses another fiber inside its body
  def test_fiber_nested_with_gc = assert_eval(<<~'RUBY', [1, 2, 3, 10, 20, 30])
    outer = Fiber.new do
      inner = Fiber.new do
        Fiber.yield 10
        Fiber.yield 20
        30
      end
      Fiber.yield 1
      i = 0
      while i < 300
        x = "nest_" + i.to_s
        i += 1
      end
      Fiber.yield 2
      Fiber.yield 3
      r = []
      r << inner.resume
      i = 0
      while i < 300
        x = "nest2_" + i.to_s
        i += 1
      end
      r << inner.resume
      r << inner.resume
      r
    end
    results = []
    results << outer.resume
    results << outer.resume
    results << outer.resume
    inner_results = outer.resume
    results + inner_results
  RUBY

  # Fiber + exception handling: fiber body raises, outer catches
  def test_fiber_exception_rescue = assert_eval(<<~'RUBY', "caught:oops")
    f = Fiber.new do
      Fiber.yield 1
      raise "oops"
    end
    f.resume
    begin
      f.resume
      "no error"
    rescue => e
      "caught:" + e.message
    end
  RUBY

  # Fiber + deep block nesting with GC pressure
  def test_fiber_deep_block_nesting = assert_eval(<<~'RUBY', 27)
    f = Fiber.new do
      count = 0
      3.times do
        3.times do
          3.times do
            count += 1
            i = 0
            while i < 50
              x = "deep_" + i.to_s
              i += 1
            end
          end
        end
      end
      count
    end
    f.resume
  RUBY

  # Fiber + class definition inside fiber body with GC pressure
  def test_fiber_class_def_in_body = assert_eval(<<~'RUBY', 55)
    f = Fiber.new do
      class FibClsA
        def initialize(v); @v = v; end
        def val; @v; end
      end
      Fiber.yield 1
      objs = []
      i = 0
      while i < 10
        objs << FibClsA.new(i)
        j = 0
        while j < 100
          x = "cls_" + j.to_s
          j += 1
        end
        i += 1
      end
      sum = 0
      i = 0
      while i < objs.length
        sum += objs[i].val
        i += 1
      end
      sum
    end
    f.resume  # yields 1, defines class
    r = f.resume  # creates objects, returns sum(0..9) = 45
    r + 10
  RUBY

  # Fiber + heavy string interpolation (many temp objects)
  def test_fiber_string_interpolation_heavy = assert_eval(<<~'RUBY', 50)
    f = Fiber.new do
      count = 0
      i = 0
      while i < 50
        s = "item_#{i}_val_#{i * 2}_end"
        t = "#{s}_extra_#{i + 1}"
        count += 1
        i += 1
      end
      Fiber.yield count
      count
    end
    f.resume
  RUBY

  # Fiber + hash/array creation with GC pressure between resumes
  def test_fiber_hash_array_gc = assert_eval(<<~'RUBY', [5, 10])
    f = Fiber.new do
      a = []
      i = 0
      while i < 5
        h = {"key" => i, "val" => "v_" + i.to_s}
        a << h
        i += 1
      end
      Fiber.yield a.length
      i = 0
      while i < 5
        h = {"k2" => i + 5, "v2" => "w_" + i.to_s}
        a << h
        i += 1
      end
      a.length
    end
    r1 = f.resume
    i = 0
    while i < 500
      x = "between_" + i.to_s
      i += 1
    end
    r2 = f.resume
    [r1, r2]
  RUBY

  # Fiber ping-pong: two fibers communicate via shared array
  def test_fiber_ping_pong = assert_eval(<<~'RUBY', [1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
    shared = []
    ping = Fiber.new do
      i = 1
      while i <= 10
        shared << i
        Fiber.yield i
        i += 2
      end
    end
    pong = Fiber.new do
      i = 2
      while i <= 10
        shared << i
        Fiber.yield i
        i += 2
      end
    end
    5.times do
      ping.resume
      pong.resume
    end
    shared
  RUBY

  # Massive abandoned fibers: create 200, resume once, abandon, GC pressure
  def test_fiber_massive_abandoned = assert_eval(<<~'RUBY', 200)
    count = 0
    i = 0
    while i < 200
      f = Fiber.new do |n|
        Fiber.yield n * 2
        999
      end
      f.resume(i)
      count += 1
      if i % 20 == 0
        j = 0
        while j < 200
          x = "abandon_" + j.to_s + "_" + i.to_s
          j += 1
        end
      end
      i += 1
    end
    count
  RUBY

  # Fiber + Proc.new inside block, call after yield
  def test_fiber_proc_in_block = assert_eval(<<~'RUBY', [10, 20])
    f = Fiber.new do
      pr = nil
      1.times do
        mul = 10
        pr = Proc.new { |x| x * mul }
      end
      Fiber.yield pr.call(1)
      i = 0
      while i < 300
        x = "proc_alloc_" + i.to_s
        i += 1
      end
      pr.call(2)
    end
    r1 = f.resume
    r2 = f.resume
    [r1, r2]
  RUBY

  # Long-running fibonacci generator with GC pressure every 10 resumes
  def test_fiber_fib_generator_100 = assert_eval(<<~'RUBY', 218922995834555169026)
    fib = Fiber.new do
      a, b = 0, 1
      loop do
        Fiber.yield a
        a, b = b, a + b
      end
    end
    result = nil
    i = 0
    while i < 100
      result = fib.resume
      if i % 10 == 0
        j = 0
        while j < 200
          x = "fib_gc_" + j.to_s
          j += 1
        end
      end
      i += 1
    end
    result
  RUBY

  # Fiber yielding from nested method calls with GC pressure
  def test_fiber_yield_nested_methods_gc = assert_eval(<<~'RUBY', [1, 2, 3, 4, 5])
    def level3(n)
      Fiber.yield n
      n
    end
    def level2(n)
      level3(n)
    end
    def level1(n)
      level2(n)
    end
    f = Fiber.new do
      i = 1
      while i <= 5
        level1(i)
        j = 0
        while j < 100
          x = "nested_" + j.to_s
          j += 1
        end
        i += 1
      end
    end
    out = []
    5.times { out << f.resume }
    out
  RUBY

  # Fiber + multiple classes interacting under GC pressure
  def test_fiber_multi_class_gc = assert_eval(<<~'RUBY', 90)
    class FibPairA
      def initialize(x, y); @x = x; @y = y; end
      def sum; @x + @y; end
    end
    f = Fiber.new do
      total = 0
      i = 0
      while i < 10
        p = FibPairA.new(i, i)
        total += p.sum
        j = 0
        while j < 100
          x = "pair_" + j.to_s
          j += 1
        end
        Fiber.yield total
        i += 1
      end
      total
    end
    result = nil
    10.times do
      result = f.resume
    end
    result
  RUBY

  # Rapid create-resume-abandon cycle
  def test_fiber_rapid_cycle = assert_eval(<<~'RUBY', 500)
    total = 0
    i = 0
    while i < 500
      f = Fiber.new { |n| n + 1 }
      total += 1 if f.resume(i) == i + 1
      i += 1
    end
    total
  RUBY

  # Fiber with large local variable count
  def test_fiber_many_locals = assert_eval(<<~'RUBY', 55)
    f = Fiber.new do
      a = 1; b = 2; c = 3; d = 4; e = 5
      f2 = 6; g = 7; h = 8; i2 = 9; j = 10
      Fiber.yield a + b + c + d + e
      k = 0
      while k < 200
        x = "loc_" + k.to_s
        k += 1
      end
      f2 + g + h + i2 + j
    end
    r1 = f.resume  # 15
    r2 = f.resume  # 40
    r1 + r2
  RUBY

  # Fiber with array accumulation across yields
  def test_fiber_array_across_yields = assert_eval(<<~'RUBY', [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
    f = Fiber.new do
      arr = []
      i = 0
      while i < 10
        arr << i
        Fiber.yield arr.length
        j = 0
        while j < 100
          x = "arr_" + j.to_s
          j += 1
        end
        i += 1
      end
      arr
    end
    10.times { f.resume }
    f.resume
  RUBY

  # Multiple fibers with interleaved GC pressure
  def test_fiber_interleaved_gc = assert_eval(<<~'RUBY', [10, 100, 20, 200, 30, 300])
    f1 = Fiber.new do
      Fiber.yield 10
      Fiber.yield 20
      30
    end
    f2 = Fiber.new do
      Fiber.yield 100
      Fiber.yield 200
      300
    end
    out = []
    3.times do
      out << f1.resume
      j = 0
      while j < 200
        x = "ilv_" + j.to_s
        j += 1
      end
      out << f2.resume
      j = 0
      while j < 200
        x = "ilv2_" + j.to_s
        j += 1
      end
    end
    out
  RUBY
end
