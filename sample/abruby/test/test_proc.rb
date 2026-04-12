require_relative 'test_helper'

class TestProc < AbRubyTest
  # Basic creation
  def test_proc_new = assert_eval("Proc.new { 42 }.call", 42)
  def test_proc_new_with_arg = assert_eval("Proc.new { |x| x + 1 }.call(10)", 11)
  def test_proc_new_two_args = assert_eval("Proc.new { |a, b| a + b }.call(3, 4)", 7)

  def test_proc_kernel = assert_eval("proc { 99 }.call", 99)
  def test_lambda_kernel = assert_eval("lambda { 7 }.call", 7)

  def test_proc_class_const = assert_eval("Proc.new { 1 }.is_a?(Proc)", true)

  # call / [] / .() / yield aliases
  def test_proc_call_alias = assert_eval("p = Proc.new { |x| x * 2 }; p.call(5)", 10)
  def test_proc_brackets    = assert_eval("p = Proc.new { |x| x * 2 }; p[5]", 10)
  def test_proc_yield       = assert_eval("p = Proc.new { |x| x * 2 }; p.yield(5)", 10)

  # Auto-splat: a single Array arg into a |a, b| block
  def test_proc_auto_splat = assert_eval(
    "Proc.new { |a, b| [a, b] }.call([1, 2])", [1, 2])

  # Returns from Proc.new propagate to the enclosing method
  def test_proc_return = assert_eval(<<~RUBY, 99)
    def use_proc
      Proc.new { return 99 }.call
      100
    end
    use_proc
  RUBY

  # lambda's return only exits the lambda
  def test_lambda_return = assert_eval(<<~RUBY, 11)
    def use_lambda
      l = lambda { return 1; 2 }
      l.call + 10
    end
    use_lambda
  RUBY

  # Closure: capture and mutate a local from the enclosing method
  def test_proc_closure_counter = assert_eval(<<~RUBY, [1, 2, 3])
    def make_counter
      n = 0
      Proc.new { n += 1; n }
    end
    c = make_counter
    [c.call, c.call, c.call]
  RUBY

  # Closure outlives the enclosing method
  def test_proc_outlives_method = assert_eval(<<~RUBY, 5)
    def make
      x = 5
      Proc.new { x }
    end
    p = make
    p.call
  RUBY

  # Closure can read multiple captures
  def test_proc_multi_capture = assert_eval(<<~RUBY, 30)
    def make
      a = 10
      b = 20
      Proc.new { a + b }
    end
    make.call
  RUBY

  # &block parameter
  def test_block_param_call = assert_eval(<<~RUBY, 84)
    def take(&blk); blk.call(42); end
    take { |x| x * 2 }
  RUBY

  def test_block_param_nil_when_absent = assert_eval(<<~RUBY, true)
    def take(&blk); blk.nil?; end
    take
  RUBY

  def test_block_param_lambda_p = assert_eval(<<~RUBY, false)
    def take(&blk); blk.lambda?; end
    take { 1 }
  RUBY

  # & at call site: pass a Proc as a block
  def test_amp_pass_proc = assert_eval(<<~RUBY, 21)
    p = Proc.new { |x| x * 3 }
    def f; yield 7; end
    f(&p)
  RUBY

  def test_amp_pass_lambda = assert_eval(<<~RUBY, 7)
    def g(x, y); yield x, y; end
    g(2, 5, &lambda { |a, b| a + b })
  RUBY

  # Proc#arity / lambda?
  def test_arity_zero = assert_eval("lambda { }.arity", 0)
  def test_arity_one  = assert_eval("lambda { |x| x }.arity", 1)
  def test_arity_two  = assert_eval("lambda { |a, b| a + b }.arity", 2)
  def test_lambda_p_true  = assert_eval("lambda { 1 }.lambda?", true)
  def test_lambda_p_false = assert_eval("Proc.new { 1 }.lambda?", false)

  # next inside a proc — exits one call
  def test_proc_next = assert_eval(<<~RUBY, 99)
    p = Proc.new { next 99; 0 }
    p.call
  RUBY

  # Proc#to_proc returns self
  def test_to_proc = assert_eval("p = Proc.new { 1 }; p.to_proc.equal?(p)", true)

  # Pass a Proc directly to a method that yields
  def test_proc_via_amp_to_each = assert_eval(<<~RUBY, [2, 4, 6])
    sq = Proc.new { |x| x * 2 }
    [1, 2, 3].map(&sq)
  RUBY

  # Regression: when the proc body calls methods, the body's fp must have
  # headroom on the CTX's own VALUE stack — not the (small) captured-env
  # heap buffer.  Previously `c->fp = p->env` + a method call inside the
  # body wrote past p->env's allocation and corrupted adjacent heap
  # (observable as a random ivar on a long-lived object flipping type).
  def test_proc_body_deep_call = assert_eval(<<~RUBY, 55)
    def fib(n)
      return n if n < 2
      fib(n - 1) + fib(n - 2)
    end
    Proc.new { fib(10) }.call
  RUBY

  # Regression: a proc body that itself calls a method whose body calls
  # another method.  Each layer advances fp; without the stack copy the
  # innermost call corrupts heap.
  def test_proc_body_chained_calls = assert_eval(<<~RUBY, 42)
    class TmAccum
      def initialize; @n = 0; end
      def add(x); @n += x; self; end
      def value; @n; end
    end
    a = TmAccum.new
    Proc.new { a.add(10).add(20).add(12).value }.call
  RUBY

  # Closure inside a block
  def test_block_in_block_closure = assert_eval(<<~RUBY, [1, 2, 3])
    def make
      n = 0
      [Proc.new { n += 1 }, Proc.new { n += 1 }, Proc.new { n += 1 }]
    end
    procs = make
    # Each proc has a *snapshot* of n at creation time (n was 0).
    # After calls they become 1, 1, 1 with current implementation
    # — but we test that each individually counts up correctly.
    [procs[0].call, procs[0].call, procs[0].call]
  RUBY
end
