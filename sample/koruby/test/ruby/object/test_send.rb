# Tests for Object#send / __send__

require_relative "../../test_helper"

class S
  def hello; "hi"; end
  def greet(name); "hi, #{name}"; end
  def add(a, b); a + b; end
end

def test_send_no_args
  assert_equal "hi", S.new.send(:hello)
end

def test_send_with_arg
  assert_equal "hi, bob", S.new.send(:greet, "bob")
end

def test_send_two_args
  assert_equal 7, S.new.send(:add, 3, 4)
end

def test_send_with_string_name
  assert_equal "hi", S.new.send("hello")
end

def test_send_underscored
  assert_equal "hi", S.new.__send__(:hello)
end

def test_send_to_builtin
  assert_equal 3, [1, 2, 3].send(:size)
  assert_equal "1,2,3", [1, 2, 3].send(:join, ",")
end

# send hits private methods (in CRuby) — not strict here
def test_send_with_block
  result = [1, 2, 3].send(:map) { |x| x * 2 }
  assert_equal [2, 4, 6], result
end

TESTS = [
  :test_send_no_args,
  :test_send_with_arg,
  :test_send_two_args,
  :test_send_with_string_name,
  :test_send_underscored,
  :test_send_to_builtin,
  :test_send_with_block,
]

TESTS.each { |t| run_test(t) }
report "Send"
