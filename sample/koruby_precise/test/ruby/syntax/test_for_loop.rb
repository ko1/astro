require_relative "../../test_helper"

def test_for_array
  out = []
  for x in [1, 2, 3]
    out << x * 10
  end
  assert_equal [10, 20, 30], out
end

def test_for_range
  s = 0
  for i in 1..5
    s += i
  end
  assert_equal 15, s
end

def test_for_does_not_introduce_scope
  # for ループの index 変数は外側スコープに残る (Ruby 仕様: for は新しいスコープを作らない)
  for k in [10, 20, 30]
    # body
  end
  assert_equal 30, k
end

TESTS = [:test_for_array, :test_for_range, :test_for_does_not_introduce_scope]
TESTS.each { |t| run_test(t) }
report "ForLoop"
