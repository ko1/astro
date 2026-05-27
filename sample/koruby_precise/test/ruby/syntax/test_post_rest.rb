require_relative "../../test_helper"

# def f(a, *rest, b)
def head_rest_tail(a, *r, z)
  [a, r, z]
end

def test_basic_post_rest
  assert_equal [1, [2, 3, 4], 5], head_rest_tail(1, 2, 3, 4, 5)
end

def test_no_rest_elements
  # Only required + tail
  assert_equal [1, [], 2], head_rest_tail(1, 2)
end

def test_one_rest_element
  assert_equal [1, [2], 3], head_rest_tail(1, 2, 3)
end

# Two pre, *rest, one post
def two_pre(a, b, *r, z)
  [a, b, r, z]
end

def test_two_pre
  assert_equal [1, 2, [3, 4], 5], two_pre(1, 2, 3, 4, 5)
  assert_equal [1, 2, [], 3],     two_pre(1, 2, 3)
end

# rest only at end (existing behavior, sanity check)
def rest_only(*r)
  r
end

def test_rest_only_still_works
  assert_equal [], rest_only
  assert_equal [1, 2, 3], rest_only(1, 2, 3)
end

TESTS = [
  :test_basic_post_rest,
  :test_no_rest_elements,
  :test_one_rest_element,
  :test_two_pre,
  :test_rest_only_still_works,
]

TESTS.each { |t| run_test(t) }
report "PostRest"
