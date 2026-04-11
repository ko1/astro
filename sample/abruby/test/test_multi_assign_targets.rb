require_relative 'test_helper'

class TestMultiAssignTargets < AbRubyTest
  # IndexTargetNode: `a[i], b[j] = x, y`
  def test_multi_assign_index_pair = assert_eval(<<~RUBY, [10, 20])
    a = [0, 0]
    a[0], a[1] = 10, 20
    a
  RUBY

  def test_multi_assign_index_mixed = assert_eval(<<~RUBY, [99, 100, 0])
    a = [0, 0, 0]
    x = 99
    x, a[1], a[0] = x, 100, 99
    a
  RUBY

  # CallTargetNode: `obj.x, obj.y = ...`
  def test_multi_assign_attr = assert_eval(<<~RUBY, [5, 6])
    class C
      attr_accessor :x, :y
    end
    c = C.new
    c.x, c.y = 5, 6
    [c.x, c.y]
  RUBY

  # Mixed: local + index target
  def test_multi_assign_local_and_index = assert_eval(<<~RUBY, [7, [8, 9]])
    a = [0, 0]
    x, a[0], a[1] = 7, 8, 9
    [x, a]
  RUBY
end

class TestExceptionHierarchy < AbRubyTest
  # All the aliases exist and equal RuntimeError.
  def test_standard_error_alias = assert_eval("StandardError == RuntimeError", true)
  def test_exception_alias      = assert_eval("Exception == RuntimeError", true)
  def test_nie_alias            = assert_eval("NotImplementedError == RuntimeError", true)
  def test_arg_error_alias      = assert_eval("ArgumentError == RuntimeError", true)

  # Subclassing an alias works and rescue still catches.
  def test_subclass_standard_error = assert_eval(<<~RUBY, "caught: oops")
    class MyError < StandardError; end
    begin
      raise MyError, "oops"
    rescue => e
      "caught: \#{e.message}"
    end
  RUBY

  # 2-arg raise: (class, message)
  def test_raise_two_args = assert_eval(<<~RUBY, "boom")
    begin
      raise ArgumentError, "boom"
    rescue => e
      e.message
    end
  RUBY

  # Re-raise of exception instance
  def test_reraise_instance = assert_eval(<<~RUBY, "first")
    msg = nil
    begin
      begin
        raise "first"
      rescue => e
        raise e
      end
    rescue => e2
      msg = e2.message
    end
    msg
  RUBY
end
