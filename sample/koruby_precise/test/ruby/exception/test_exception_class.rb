require_relative "../../test_helper"

def test_raise_class_with_message
  begin
    raise ArgumentError, "bad arg"
  rescue ArgumentError => e
    assert_equal "bad arg", e.message
    assert_equal ArgumentError, e.class
  end
end

def test_rescue_by_superclass
  caught = false
  begin
    raise TypeError, "wrong type"
  rescue StandardError => e
    caught = true
    assert_equal "wrong type", e.message
  end
  assert_equal true, caught
end

def test_raise_string_is_runtime_error
  begin
    raise "plain"
  rescue => e
    assert_equal RuntimeError, e.class
    assert_equal "plain", e.message
  end
end

def test_exception_to_s
  begin
    raise "foo"
  rescue => e
    assert_equal "foo", e.to_s
  end
end

def test_user_exception_subclass
  custom = Class.new(StandardError)
  begin
    raise custom, "custom msg"
  rescue => e
    assert_equal "custom msg", e.message
    assert_equal true, e.is_a?(StandardError)
  end
end

def test_rescue_specific_no_match_propagates
  caught_outer = false
  begin
    begin
      raise TypeError, "inner"
    rescue ArgumentError
      assert_equal :should_not_get_here, true
    end
  rescue TypeError => e
    caught_outer = true
    assert_equal "inner", e.message
  end
  assert_equal true, caught_outer
end

TESTS = [
  :test_raise_class_with_message,
  :test_rescue_by_superclass,
  :test_raise_string_is_runtime_error,
  :test_exception_to_s,
  :test_user_exception_subclass,
  :test_rescue_specific_no_match_propagates,
]

TESTS.each { |t| run_test(t) }
report "ExceptionClass"
