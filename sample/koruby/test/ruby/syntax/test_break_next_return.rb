require_relative "../../test_helper"

# break / next / return semantics across nested loops and blocks.

# ---------- break ----------

def test_break_returns_value
  result = [1, 2, 3].each { |x| break x * 10 if x == 2 }
  assert_equal 20, result
end

def test_break_inner_loop_only
  outer = []
  3.times do |i|
    3.times do |j|
      break if j == 1
      outer << [i, j]
    end
  end
  assert_equal [[0, 0], [1, 0], [2, 0]], outer
end

# ---------- next ----------

def test_next_skips_current
  out = []
  [1, 2, 3, 4].each { |x| next if x.even?; out << x }
  assert_equal [1, 3], out
end

def test_next_with_value_in_map
  result = [1, 2, 3].map { |x| next 99 if x == 2; x }
  assert_equal [1, 99, 3], result
end

# ---------- return from method via block ----------

def helper_with_block
  [1, 2, 3].each { |x| return x * 100 if x == 2 }
  :unreached
end

def test_block_return_unwinds_method
  assert_equal 200, helper_with_block
end

# ---------- while loop break ----------

def test_while_break_with_value
  i = 0
  result = while i < 10
    break "stop:#{i}" if i == 3
    i += 1
  end
  assert_equal "stop:3", result
end

# ---------- next inside while ----------

def test_next_in_while
  out = []
  i = 0
  while i < 5
    i += 1
    next if i == 3
    out << i
  end
  assert_equal [1, 2, 4, 5], out
end

TESTS = [
  :test_break_returns_value,
  :test_break_inner_loop_only,
  :test_next_skips_current,
  :test_next_with_value_in_map,
  :test_block_return_unwinds_method,
  :test_while_break_with_value,
  :test_next_in_while,
]
TESTS.each { |t| run_test(t) }
report "BreakNextReturn"
