require_relative "../../test_helper"

LINE_AT_3 = __LINE__
def line_in_method
  __LINE__
end

def test_line_constant
  assert_equal 3, LINE_AT_3
end

def test_line_in_method
  # body of line_in_method is at line 5
  assert_equal 5, line_in_method
end

def test_file_is_path
  # __FILE__ returns the full filename of this script.
  assert_equal true, __FILE__.end_with?("test_source_loc.rb")
end

TESTS = [:test_line_constant, :test_line_in_method, :test_file_is_path]
TESTS.each { |t| run_test(t) }
report "SourceLoc"
