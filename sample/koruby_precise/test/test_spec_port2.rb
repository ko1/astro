# Second spec-port batch — focused on control-flow / closure / dispatch paths,
# the subsystem where the remaining moving-GC rooting gaps cluster (method
# dispatch, blocks, procs, exceptions).  Run normal AND under
# ASTRO_GC_STRESS=1 ASTRO_GC_PURGE=1.
require_relative "test_helper"

# --- Proc / lambda / closures (proc_spec.rb, lambda_spec.rb) ---
def test_proc_basic
  add = ->(a, b) { a + b }
  assert_equal 5, add.call(2, 3)
  assert_equal 5, add.(2, 3)
  assert_equal 5, add[2, 3]
  double = proc { |x| x * 2 }
  assert_equal [2, 4, 6], [1, 2, 3].map(&double)
  counter = 0
  inc = -> { counter += 1 }
  inc.call
  inc.call
  assert_equal 2, counter
end

# --- closures capturing & mutating ---
# A stored Proc/lambda that captures a moving Array and mutates it with `<<`
# across repeated `.call` from inside another block used to crash under
# STRESS+PURGE: EVAL_node_lshift's Array fast-path returned the stale C-local
# `l` after korb_ary_push relocated it.  Fixed by returning the parked
# (forwarded) slot — now exercised here in both modes.
def test_closure_capture
  acc = []
  collect = ->(x) { acc << x }
  [1, 2, 3].each { |x| collect.call(x) }
  assert_equal [1, 2, 3], acc
  doubler = ->(x) { acc << x * 2 }
  [10, 20].each { |x| doubler.call(x) }
  assert_equal [1, 2, 3, 20, 40], acc
  sum = 0
  [10, 20, 30].each { |x| sum += x }
  assert_equal 60, sum
  # direct (non-stored-proc) block accumulation works:
  strs = []
  (1..5).each { |i| strs << "n#{i}" }
  assert_equal ["n1", "n2", "n3", "n4", "n5"], strs
  # immediate-capturing closure (no moving handle) works:
  base = 100
  addn = ->(x) { base + x }
  assert_equal [101, 102, 103], [1, 2, 3].map { |x| addn.call(x) }
end

# --- method calls / send / respond_to? (send_spec.rb) ---
def test_method_dispatch
  assert_equal 3, "abc".length
  assert_equal 3, "abc".send(:length)
  assert_equal true, "abc".respond_to?(:length)
  assert_equal false, "abc".respond_to?(:nonexistent_xyz)
  assert_equal "ABC", "abc".public_send(:upcase)
  assert_equal [1, 2, 3], [3, 1, 2].sort
end

# --- exceptions: raise / rescue / ensure / retry (rescue_spec.rb) ---
def test_exceptions
  result = begin
    raise ArgumentError, "bad"
  rescue ArgumentError => e
    e.message
  end
  assert_equal "bad", result
  ran_ensure = false
  begin
    raise "x"
  rescue
    nil
  ensure
    ran_ensure = true
  end
  assert_equal true, ran_ensure
  assert_equal 42, (begin; raise "z"; rescue; 42; end)
  klass = begin
    [].fetch(99)
  rescue IndexError => e
    e.class
  end
  assert_equal IndexError, klass
end

# --- exception with object building in rescue (stresses rooting) ---
def test_exception_rooting
  errors = []
  [1, 2, 3, 0, 4].each do |n|
    begin
      r = 10 / n
      errors << "ok#{r}"
    rescue ZeroDivisionError
      errors << "div0"
    end
  end
  assert_equal ["ok10", "ok5", "ok3", "div0", "ok2"], errors
end

# --- yield / blocks (yield_spec.rb) ---
def test_yield
  def with_yield
    yield 1
    yield 2
    yield 3
  end
  acc = []
  with_yield { |x| acc << x * 10 }
  assert_equal [10, 20, 30], acc
  def counts
    return enum_for(:counts) unless block_given?
    yield "a"
    yield "b"
  end
  got = []
  counts { |x| got << x }
  assert_equal ["a", "b"], got
end

TESTS = %i[
  test_proc_basic test_closure_capture test_method_dispatch
  test_exceptions test_exception_rooting test_yield
]
TESTS.each { |t| run_test(t) }
report("SpecPort2")
