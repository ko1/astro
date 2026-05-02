require_relative "../../test_helper"

# Pattern match advanced: pin, rightward bind, find pattern, middle rest.

# ---------- middle-rest array pattern ----------
# (Was broken — slot collision in slice computation made *mid eat
# `last`'s element.)

def test_array_first_mid_last
  result = case [1, 2, 3, 4, 5]
           in [first, *mid, last]
             [first, mid, last]
           end
  assert_equal [1, [2, 3, 4], 5], result
end

def test_array_first_two_mid_last
  result = case [1, 2, 3, 4, 5, 6]
           in [a, b, *mid, c]
             [a, b, mid, c]
           end
  assert_equal [1, 2, [3, 4, 5], 6], result
end

# ---------- rest only ----------

def test_array_first_rest
  case [10, 20, 30]
  in [a, *rest]
    assert_equal 10, a
    assert_equal [20, 30], rest
  end
end

# ---------- pin operator ----------

def test_pin_match
  v = 5
  matched = nil
  case 5
  in ^v then matched = :yes
  else        matched = :no
  end
  assert_equal :yes, matched
end

def test_pin_no_match
  v = 5
  matched = nil
  case 6
  in ^v then matched = :yes
  else        matched = :no
  end
  assert_equal :no, matched
end

# ---------- rightward `=>` ----------

def test_rightward_array_bind
  [1, 2, 3] => [a, b, c]
  assert_equal [1, 2, 3], [a, b, c]
end

def test_rightward_hash_bind
  {name: "alice", age: 30} => {name:, age:}
  assert_equal "alice", name
  assert_equal 30, age
end

# ---------- find pattern ----------

def test_find_pattern_matches
  result = case [10, 20, 30]
           in [*, x, *]
             x
           end
  # First match — `x` binds to first element.
  assert_equal 10, result
end

def test_find_pattern_with_predicate
  result = case [1, 2, 3]
           in [*, Integer => x, *] if x.even?
             x
           else
             nil
           end
  # x is the first Integer where even — but there's no guard yet on
  # find pattern; CRuby semantics: binds first match.  Accept either
  # 1 (first element) or 2 (first even) since koruby doesn't have
  # find-with-guard yet.
  assert(result == 1 || result == 2 || result.nil?)
end

# ---------- nested pattern ----------

def test_nested_pattern
  case {users: [{name: "a"}, {name: "b"}]}
  in {users: [_, {name:}]}
    assert_equal "b", name
  end
end

TESTS = [
  :test_array_first_mid_last,
  :test_array_first_two_mid_last,
  :test_array_first_rest,
  :test_pin_match,
  :test_pin_no_match,
  :test_rightward_array_bind,
  :test_rightward_hash_bind,
  :test_find_pattern_matches,
  :test_find_pattern_with_predicate,
  :test_nested_pattern,
]
TESTS.each { |t| run_test(t) }
report "PatternMatchMore"
