require_relative "../../test_helper"

# Regression: proc.call must write back to outer-scope locals.
# Earlier proc.call snapshotted env into a fresh stack frame, which
# meant assignments inside the proc body never reached the outer
# binding.  Now uses the captured env directly.

def test_proc_writes_to_outer_local
  r = nil
  p = proc { |a, b| r = [a, b] }
  p.call(1, 2)
  assert_equal [1, 2], r
end

def test_proc_chained_writes
  acc = []
  p = proc { |x| acc << x }
  p.call(:a)
  p.call(:b)
  p.call(:c)
  assert_equal [:a, :b, :c], acc
end

def test_proc_compound_assign_outer
  count = 0
  inc = proc { count += 1 }
  inc.call
  inc.call
  inc.call
  assert_equal 3, count
end

def test_proc_sees_later_outer_assignment
  x = 1
  p = proc { x }
  x = 2
  assert_equal 2, p.call
  x = 99
  assert_equal 99, p.call
end

# ---------- Lambda variant ----------

def test_lambda_writes_to_outer
  r = nil
  l = ->(v) { r = v }
  l.call(:hello)
  assert_equal :hello, r
end

TESTS = [
  :test_proc_writes_to_outer_local,
  :test_proc_chained_writes,
  :test_proc_compound_assign_outer,
  :test_proc_sees_later_outer_assignment,
  :test_lambda_writes_to_outer,
]
TESTS.each { |t| run_test(t) }
report "ProcWriteback"
