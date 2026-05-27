require_relative "../../test_helper"

# rescue clause variants, ensure semantics, catch/throw, retry.

# ---------- multiple exception classes in one rescue ----------

def test_rescue_multiple_classes
  result = begin
    raise ArgumentError, "x"
  rescue TypeError, ArgumentError => e
    e.class
  end
  assert_equal ArgumentError, result
end

# ---------- multiple rescue clauses ----------

def test_multiple_rescue_clauses
  result = begin
    raise ArgumentError, "x"
  rescue TypeError
    :type_error
  rescue ArgumentError
    :arg_error
  rescue
    :default
  end
  assert_equal :arg_error, result
end

# ---------- bare rescue catches StandardError ----------

def test_bare_rescue
  result = begin
    raise RuntimeError, "boom"
  rescue => e
    e.message
  end
  assert_equal "boom", result
end

# ---------- ensure runs on success ----------

def test_ensure_runs_on_success
  log = []
  begin
    log << :body
  ensure
    log << :ensure
  end
  assert_equal [:body, :ensure], log
end

# ---------- ensure runs on exception ----------

def test_ensure_runs_on_exception
  log = []
  begin
    begin
      log << :body
      raise "boom"
    ensure
      log << :ensure
    end
  rescue
    log << :outer
  end
  assert_equal [:body, :ensure, :outer], log
end

# ---------- ensure result is discarded ----------

def test_ensure_does_not_change_value
  result = begin
    42
  ensure
    99
  end
  assert_equal 42, result
end

# ---------- catch / throw ----------

def test_catch_throw_basic
  result = catch(:tag) do
    throw :tag, "value"
    :unreachable
  end
  assert_equal "value", result
end

def test_catch_no_throw_returns_block_result
  result = catch(:tag) { 42 }
  assert_equal 42, result
end

def test_throw_unwinds_through_loops
  result = catch(:done) do
    (1..10).each do |i|
      throw :done, i if i == 5
    end
    :not_reached
  end
  assert_equal 5, result
end

# ---------- retry inside rescue ----------

def test_retry_in_rescue
  attempts = 0
  begin
    attempts += 1
    raise "boom" if attempts < 3
  rescue
    retry if attempts < 3
  end
  assert_equal 3, attempts
end

TESTS = [
  :test_rescue_multiple_classes,
  :test_multiple_rescue_clauses,
  :test_bare_rescue,
  :test_ensure_runs_on_success,
  :test_ensure_runs_on_exception,
  :test_ensure_does_not_change_value,
  :test_catch_throw_basic,
  :test_catch_no_throw_returns_block_result,
  :test_throw_unwinds_through_loops,
  :test_retry_in_rescue,
]
TESTS.each { |t| run_test(t) }
report "RescueMore"
