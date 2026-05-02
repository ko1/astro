require_relative "../../test_helper"

# yield without a block: must raise LocalJumpError (or similar).

def yields_one_arg
  yield 42
end

def test_yield_with_block
  assert_equal 84, yields_one_arg { |x| x * 2 }
end

def test_yield_no_block_raises
  raised = false
  klass = nil
  begin
    yields_one_arg
  rescue LocalJumpError => e
    raised = true; klass = e.class
  rescue StandardError => e
    raised = true; klass = e.class
  end
  assert raised, "expected an exception when yielding with no block"
end

# ---------- block_given? ----------

def maybe_yield(default)
  if block_given?
    yield default
  else
    default
  end
end

def test_block_given_branches
  assert_equal 5,  maybe_yield(5)
  assert_equal 50, maybe_yield(5) { |x| x * 10 }
end

# ---------- yield as expression ----------

def yield_returns_block_result
  yield + 1
end

def test_yield_value
  assert_equal 11, yield_returns_block_result { 10 }
end

# ---------- yield with multiple args / array auto-pack ----------

def yields_two
  yield 1, 2
end

def test_yield_two_args_one_param_packs
  result = yields_two { |x| x }
  assert_equal [1, 2], result   # auto-pack into array
end

def test_yield_two_args_two_params
  result = yields_two { |a, b| [a, b] }
  assert_equal [1, 2], result
end

TESTS = [
  :test_yield_with_block,
  :test_yield_no_block_raises,
  :test_block_given_branches,
  :test_yield_value,
  :test_yield_two_args_one_param_packs,
  :test_yield_two_args_two_params,
]
TESTS.each { |t| run_test(t) }
report "YieldLocalJump"
