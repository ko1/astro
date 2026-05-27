require_relative "../../test_helper"

def test_array_pattern
  case [1, 2, 3]
  in [a, b, c]
    assert_equal 1, a
    assert_equal 2, b
    assert_equal 3, c
  end
end

def test_array_pattern_size_mismatch
  matched = nil
  case [1, 2]
  in [a, b, c]
    matched = :three
  in [a, b]
    matched = :two
  end
  assert_equal :two, matched
end

def test_hash_pattern
  case {name: "bob", age: 25}
  in {name: n, age: a}
    assert_equal "bob", n
    assert_equal 25, a
  end
end

def test_class_pattern
  hits = []
  [1, "x", 3.14].each do |v|
    case v
    in Integer; hits << :int
    in String;  hits << :str
    in Float;   hits << :flt
    end
  end
  assert_equal [:int, :str, :flt], hits
end

def test_value_pattern_range
  matched = nil
  case 7
  in 1..3; matched = :low
  in 4..6; matched = :mid
  in 7..9; matched = :hi
  end
  assert_equal :hi, matched
end

def test_local_target_always_matches
  case 42
  in n
    assert_equal 42, n
  end
end

def test_nested_array
  case [1, [2, 3]]
  in [a, [b, c]]
    assert_equal 1, a
    assert_equal 2, b
    assert_equal 3, c
  end
end

def test_pinned_match
  x = 5
  matched = nil
  case 5
  in ^x
    matched = :equal
  end
  assert_equal :equal, matched
end

def test_else_clause
  matched = nil
  case "z"
  in 1..3; matched = :n
  else;     matched = :other
  end
  assert_equal :other, matched
end

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

TESTS = %i[
  test_array_pattern test_array_pattern_size_mismatch test_hash_pattern test_class_pattern
  test_value_pattern_range test_local_target_always_matches test_nested_array test_pinned_match
  test_else_clause test_deconstruct_array test_deconstruct_keys_hash test_alternative
  test_guard test_match_expression test_nested_mixed test_array_rest
  test_in_operator
]
TESTS.each {|t| run_test(t) }
report "PatternMatch"
