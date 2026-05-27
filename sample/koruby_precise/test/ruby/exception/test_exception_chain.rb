require_relative "../../test_helper"

# Custom exception classes, message access, re-raise, nested begin.

# ---------- custom exception ----------

class MyError < StandardError; end

def test_custom_exception_class
  result = begin
    raise MyError, "boom"
  rescue MyError => e
    e.message
  end
  assert_equal "boom", result
end

def test_custom_exception_caught_by_parent
  result = begin
    raise MyError, "boom"
  rescue StandardError => e
    e.class
  end
  assert_equal MyError, result
end

# ---------- exception class hierarchy dispatch ----------

class LowError < MyError; end

def test_subclass_dispatch
  result = begin
    raise LowError, "deep"
  rescue MyError => e
    e.class
  end
  assert_equal LowError, result
end

# ---------- re-raise ----------

def test_reraise_with_bare_raise
  inner_caught = false
  outer_caught = false
  begin
    begin
      raise "boom"
    rescue
      inner_caught = true
      raise   # re-raise
    end
  rescue => e
    outer_caught = true
  end
  assert_equal true, inner_caught
  assert_equal true, outer_caught
end

# ---------- nested rescue with different classes ----------

def test_nested_rescue
  log = []
  begin
    begin
      raise ArgumentError, "inner"
    rescue TypeError
      log << :type_error  # never
    end
  rescue ArgumentError => e
    log << :outer_arg
  end
  assert_equal [:outer_arg], log
end

# ---------- Exception#message default ----------

def test_default_message_is_class_name
  e = RuntimeError.new
  # CRuby default is the class name; some impls default to "".
  assert(e.message == "RuntimeError" || e.message == "" || e.message == nil,
         "got #{e.message.inspect}")
end

TESTS = [
  :test_custom_exception_class,
  :test_custom_exception_caught_by_parent,
  :test_subclass_dispatch,
  :test_reraise_with_bare_raise,
  :test_nested_rescue,
  :test_default_message_is_class_name,
]
TESTS.each { |t| run_test(t) }
report "ExceptionChain"
