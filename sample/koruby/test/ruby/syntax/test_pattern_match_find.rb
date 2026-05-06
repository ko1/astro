require_relative "../../test_helper"

# Pattern matching — find pattern with n>1.

def find1(arr)
  case arr
  in [*, x, *]; x
  end
end

def find2(arr)
  case arr
  in [*, x, y, *]; [x, y]
  end
end

def find3(arr)
  case arr
  in [*, x, y, z, *]; [x, y, z]
  end
end

def test_find_n1
  assert_equal 10, find1([10, 20, 30])
  assert_equal 1, find1([1])
end

def test_find_n2
  assert_equal [10, 20], find2([10, 20, 30])
  assert_equal [1, 2], find2([1, 2])
end

def test_find_n3
  assert_equal [10, 20, 30], find3([10, 20, 30])
  assert_equal [1, 2, 3], find3([1, 2, 3, 4, 5])
end

def find2_typed(arr)
  case arr
  in [*, Integer => x, String => y, *]; [x, y]
  in _; nil
  end
end

def test_find_typed_window
  # Window must be consecutive Int+String somewhere.
  assert_equal [1, "b"], find2_typed([:a, "x", 1, "b", :c])
  assert_equal nil,      find2_typed([1, 2, 3])
  assert_equal nil,      find2_typed([:a, :b, "c"])
end

def find2_no_match(arr)
  case arr
  in [*, 99, 100, *]; :match
  in _; :no_match
  end
end

def test_find_no_match
  assert_equal :no_match, find2_no_match([1, 2, 3])
  assert_equal :match,    find2_no_match([1, 99, 100, 2])
end

# n>3 sanity
def test_find_n4
  r = nil
  case [1, 2, 3, 4, 5]
  in [*, a, b, c, d, *]; r = [a, b, c, d]
  end
  assert_equal [1, 2, 3, 4], r
end

# Find pattern + guard — koruby weaves the guard into the per-window
# scan so a failing guard makes the loop keep looking for a window
# where guard succeeds.
def find_g(arr)
  case arr
  in [*, x, y, *] if x + y > 10; [x, y]
  in _; nil
  end
end

def test_find_with_guard_matches_later_window
  # Skip first failing window, find later matching one.
  assert_equal [5, 8],   find_g([1, 5, 8, 2])
  assert_equal [10, 1],  find_g([10, 1, 1, 10])
end

def test_find_with_guard_no_match
  assert_equal nil, find_g([1, 2, 3])
end

def test_find_with_guard_first_window_matches
  # When the very first window already satisfies guard, take it.
  assert_equal [1, 100], find_g([1, 100, 1])
end

# Find pattern + unless guard
def find_u(arr)
  case arr
  in [*, x, y, *] unless x == y; [x, y]
  end
end

def test_find_with_unless_guard
  # Skip equal pairs.
  assert_equal [1, 2], find_u([1, 1, 1, 2, 3])
end

TESTS = %i[
  test_find_n1 test_find_n2 test_find_n3
  test_find_typed_window test_find_no_match test_find_n4
  test_find_with_guard_matches_later_window
  test_find_with_guard_no_match
  test_find_with_guard_first_window_matches
  test_find_with_unless_guard
]
TESTS.each {|t| run_test(t) }
report "FindPattern"
