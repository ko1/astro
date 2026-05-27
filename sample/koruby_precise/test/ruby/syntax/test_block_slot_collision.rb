require_relative "../../test_helper"

# Regression: block-as-arg with a Proc-typed `&block` corrupted the
# preceding arg.  Root cause was that pop_frame propagated a child
# block's max_cnt up to the parent but didn't bump arg_index, so a
# later sibling expression allocated slots that overlapped the
# captured block's param_base.  When the block was invoked later via
# yield, it wrote its param into a slot the caller had stored an arg
# in — silently corrupting it to the last yielded value.

# ---------- minimal reproducer (Proc as &block) ----------

def test_arg_then_proc_amp_call
  def f(a, b); [a, b]; end
  doubler = ->(x) { x * 2 }
  assert_equal ["x", [2, 4, 6]], f("x", [1, 2, 3].map(&doubler))
end

def test_arg_then_proc_amp_with_var_recv
  def f(a, b); [a, b]; end
  doubler = ->(x) { x * 100 }
  arr = [7, 8, 9]
  assert_equal ["pre", [700, 800, 900]], f("pre", arr.map(&doubler))
end

# ---------- multiple sibling block-arg calls ----------

def test_multiple_sibling_amp_calls
  def f(a, b); [a, b]; end
  d1 = ->(x) { x + 10 }
  d2 = ->(x) { x + 100 }
  r1 = f("a", [1, 2].map(&d1))
  r2 = f("b", [1, 2].map(&d2))
  assert_equal ["a", [11, 12]], r1
  assert_equal ["b", [101, 102]], r2
end

# ---------- nested ----------

def test_nested_amp_calls
  def f(a, b); [a, b]; end
  d = ->(x) { x * 2 }
  result = f("outer", f("inner", [1, 2].map(&d)))
  assert_equal ["outer", ["inner", [2, 4]]], result
end

TESTS = [
  :test_arg_then_proc_amp_call,
  :test_arg_then_proc_amp_with_var_recv,
  :test_multiple_sibling_amp_calls,
  :test_nested_amp_calls,
]
TESTS.each { |t| run_test(t) }
report "BlockSlotCollision"
