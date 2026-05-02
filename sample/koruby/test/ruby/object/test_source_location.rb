require_relative "../../test_helper"

# Method#source_location / Proc#source_location return [file, line]
# for AST-defined methods, lambdas, and procs.  cfunc methods return nil.

class SLHost
  def foo
    42
  end

  def empty; end

  def multi_line(
    a,
    b
  )
    a + b
  end
end

def test_method_returns_pair
  result = SLHost.new.method(:foo).source_location
  assert result.is_a?(Array), "should be an Array"
  assert_equal 2, result.length
end

def test_method_file_path
  file, _line = SLHost.new.method(:foo).source_location
  assert file.is_a?(String), "file should be a String"
  assert(file.end_with?("test_source_location.rb"), "file should end with test_source_location.rb")
end

def test_method_line_is_def_line
  # `def foo` is on line 7 of this file (after the class line).
  _file, line = SLHost.new.method(:foo).source_location
  assert_equal 7, line
end

def test_empty_def_has_line
  # Even an empty `def empty; end` should report its def line, not 0.
  _file, line = SLHost.new.method(:empty).source_location
  assert_equal 11, line
end

def test_multi_line_def_uses_def_keyword_line
  _file, line = SLHost.new.method(:multi_line).source_location
  assert_equal 13, line
end

def test_lambda_source_location
  l = ->(x) { x + 1 }
  result = l.source_location
  assert result.is_a?(Array)
  assert_equal 2, result.length
  assert(result[0].end_with?("test_source_location.rb"))
end

def test_proc_source_location
  p = proc { |x| x * 2 }
  result = p.source_location
  assert result.is_a?(Array)
  assert_equal 2, result.length
  assert(result[0].end_with?("test_source_location.rb"))
end

def test_cfunc_method_returns_nil
  # `5.method(:abs)` is a cfunc — no source location.
  result = 5.method(:abs).source_location
  assert_equal nil, result
end

TESTS = [
  :test_method_returns_pair,
  :test_method_file_path,
  :test_method_line_is_def_line,
  :test_empty_def_has_line,
  :test_multi_line_def_uses_def_keyword_line,
  :test_lambda_source_location,
  :test_proc_source_location,
  :test_cfunc_method_returns_nil,
]
TESTS.each { |t| run_test(t) }
report "SourceLocation"
