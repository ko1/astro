require_relative "../../test_helper"

def test_retry_eventually_succeeds
  n = 0
  begin
    n += 1
    raise "boom" if n < 3
  rescue
    retry
  end
  assert_equal 3, n
end

def test_retry_with_class_filter
  attempts = 0
  begin
    attempts += 1
    raise ArgumentError, "x" if attempts < 2
  rescue ArgumentError
    retry
  end
  assert_equal 2, attempts
end

TESTS = [:test_retry_eventually_succeeds, :test_retry_with_class_filter]
TESTS.each { |t| run_test(t) }
report "Retry"
