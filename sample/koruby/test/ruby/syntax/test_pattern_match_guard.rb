require_relative "../../test_helper"

# case/in with guards across mixed pattern types — exercises that a
# failed arm doesn't leak its coerced view into subsequent arms.

def classify(v)
  case v
  in [Integer => x, Integer => y] if x == y
    "diag #{x}"
  in [x, y]
    "pair (#{x},#{y})"
  in {x: Integer => x, y: Integer => y, **rest} if rest.empty?
    "exact (#{x},#{y})"
  in {x: Integer => x, y: Integer => y}
    "loose (#{x},#{y})"
  in [a, *rest] if rest.size > 2
    "long #{a}"
  in nil
    "nil"
  in _
    "other"
  end
end

def test_array_diag
  assert_equal "diag 3", classify([3, 3])
end

def test_array_pair
  assert_equal "pair (1,2)", classify([1, 2])
end

def test_hash_exact
  assert_equal "exact (1,2)", classify({x: 1, y: 2})
end

def test_hash_loose
  assert_equal "loose (1,2)", classify({x: 1, y: 2, z: 3})
end

def test_array_long
  assert_equal "long a", classify([:a, 1, 2, 3])
end

def test_nil_arm
  assert_equal "nil", classify(nil)
end

def test_other_arm
  assert_equal "other", classify("hello")
end

# Guard with destructured names must be visible.
def points
  [{kind: :p, val: 5}, {kind: :p, val: 10}, {kind: :q, val: 1}]
end

def test_guard_uses_destructured
  match = nil
  case points.first
  in {kind: :p, val: Integer => v} if v < 7
    match = "small p:#{v}"
  end
  assert_equal "small p:5", match
end

# Nested array-in-hash with guard
def test_nested_with_guard
  data = {users: [{name: "alice", age: 20}, {name: "bob", age: 17}]}
  adult_names = []
  data[:users].each do |u|
    case u
    in {name: String => name, age: Integer => age} if age >= 18
      adult_names << name
    in _
    end
  end
  assert_equal ["alice"], adult_names
end

TESTS = %i[
  test_array_diag test_array_pair test_hash_exact test_hash_loose
  test_array_long test_nil_arm test_other_arm
  test_guard_uses_destructured test_nested_with_guard
]
TESTS.each {|t| run_test(t) }
report "PatternMatchGuard"
