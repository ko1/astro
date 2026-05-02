require_relative "../../test_helper"

def test_caller_returns_array
  a = caller
  assert_equal true, a.is_a?(Array)
end

def deeply
  raise "boom"
end

def via_one
  deeply
end

def test_backtrace_has_call_chain
  begin
    via_one
  rescue => e
    bt = e.backtrace
    assert_equal true, bt.is_a?(Array)
    assert_equal true, bt.size >= 2
    # each entry is a String
    bt.each { |s| assert_equal true, s.is_a?(String) }
  end
end

def test_backtrace_includes_filename
  begin
    via_one
  rescue => e
    bt = e.backtrace
    found = bt.any? { |s| s.include?("test_backtrace.rb") }
    assert_equal true, found
  end
end

def test_backtrace_has_method_name
  begin
    via_one
  rescue => e
    bt = e.backtrace
    found = bt.any? { |s| s.include?("via_one") }
    assert_equal true, found
  end
end

TESTS = [
  :test_caller_returns_array,
  :test_backtrace_has_call_chain,
  :test_backtrace_includes_filename,
  :test_backtrace_has_method_name,
]
TESTS.each { |t| run_test(t) }
report "Backtrace"
