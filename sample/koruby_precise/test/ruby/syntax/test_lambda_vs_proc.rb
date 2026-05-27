require_relative "../../test_helper"

# Lambda vs Proc: arity strictness, return semantics.

def test_lambda_strict_arity
  l = ->(a, b) { a + b }
  raised = false
  begin
    l.call(1)            # too few args
  rescue ArgumentError
    raised = true
  end
  assert raised, "lambda should raise on wrong arity"
end

def test_proc_lenient_arity
  p = proc { |a, b| [a, b] }
  # Procs auto-pad with nil for missing args; with extra args, ignore them.
  result = p.call(1)
  assert_equal [1, nil], result
  result = p.call(1, 2, 3)
  assert_equal [1, 2], result
end

# ---------- lambda? ----------

def test_lambda_predicate
  assert_equal true,  ->(x) { x }.lambda?
  assert_equal false, proc { |x| x }.lambda?
end

# ---------- arity ----------

def test_arity
  assert_equal 0,  proc { }.arity
  assert_equal 2,  proc { |a, b| }.arity
  assert_equal -1, proc { |*a| }.arity
  assert_equal -2, proc { |a, *b| }.arity   # -(required+1)
end

# ---------- proc { |x| } returning from outer method ----------

def helper_with_proc_return
  p = proc { return 42 }
  p.call
  :unreached  # proc.return unwinds the enclosing method
end

def test_proc_return_unwinds_method
  assert_equal 42, helper_with_proc_return
end

# ---------- lambda return is local to lambda ----------

def helper_with_lambda_return
  l = ->() { return 42 }
  v = l.call
  [:after, v]
end

def test_lambda_return_local
  assert_equal [:after, 42], helper_with_lambda_return
end

TESTS = [
  :test_lambda_strict_arity,
  :test_proc_lenient_arity,
  :test_lambda_predicate,
  :test_arity,
  :test_proc_return_unwinds_method,
  :test_lambda_return_local,
]
TESTS.each { |t| run_test(t) }
report "LambdaVsProc"
