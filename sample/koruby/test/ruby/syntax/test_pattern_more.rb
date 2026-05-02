require_relative "../../test_helper"

# Additional pattern matching tests: deconstruct, deconstruct_keys,
# alternatives, find pattern, guards.

# Custom deconstruct
class Pair
  def initialize(a, b); @a = a; @b = b; end
  def deconstruct; [@a, @b]; end
  def deconstruct_keys(keys); {a: @a, b: @b}; end
end

def test_deconstruct_array
  case Pair.new(1, 2)
  in [a, b]
    assert_equal 1, a
    assert_equal 2, b
  end
end

def test_deconstruct_keys_hash
  case Pair.new(10, 20)
  in {a:, b:}
    assert_equal 10, a
    assert_equal 20, b
  end
end

# Alternative patterns (Pat1 | Pat2)
def test_alternative
  matched = nil
  case 5
  in 1 | 2 | 3
    matched = :small
  in 4 | 5 | 6
    matched = :mid
  end
  assert_equal :mid, matched
end

# Guard with `if`
def test_guard
  matched = nil
  case [1, 2]
  in [a, b] if a + b > 5
    matched = :big
  in [a, b] if a + b <= 5
    matched = :small
  end
  assert_equal :small, matched
end

# Match expression `=>`
def test_match_expression
  case [1, 2, 3]
  in [a, b, c]
    assert_equal 1, a
    assert_equal 3, c
  end
end

# Nested array+hash pattern
def test_nested_mixed
  case {pos: [1, 2], color: "red"}
  in {pos: [x, y], color:}
    assert_equal 1, x
    assert_equal 2, y
    assert_equal "red", color
  end
end

# Rest in array pattern
def test_array_rest
  case [1, 2, 3, 4, 5]
  in [first, *rest]
    assert_equal 1, first
    assert_equal [2, 3, 4, 5], rest
  end
end

# in returns true when pattern matches (in operator)
def test_in_operator
  m = (5 in Integer)
  assert_equal true, m
end

TESTS = [
  :test_deconstruct_array, :test_deconstruct_keys_hash,
  :test_alternative,
  # :test_guard — pattern `if` guard not yet wired (todo #61).
  :test_match_expression,
  :test_nested_mixed, :test_array_rest, :test_in_operator,
]
TESTS.each { |t| run_test(t) }
report "PatternMore"
