require_relative "../../test_helper"

# Hash insertion-order preservation (Ruby 1.9+ guarantee).

def test_keys_in_insertion_order
  h = {}
  h[:c] = 3
  h[:a] = 1
  h[:b] = 2
  assert_equal [:c, :a, :b], h.keys
end

def test_each_in_insertion_order
  h = {}
  h[:z] = 26; h[:a] = 1; h[:m] = 13
  out = []
  h.each { |k, _| out << k }
  assert_equal [:z, :a, :m], out
end

def test_overwrite_does_not_reorder
  h = {a: 1, b: 2, c: 3}
  h[:b] = 99
  assert_equal [:a, :b, :c], h.keys
  assert_equal 99,            h[:b]
end

def test_delete_then_reinsert_appends_at_end
  h = {a: 1, b: 2, c: 3}
  h.delete(:b)
  h[:b] = 99
  assert_equal [:a, :c, :b], h.keys
end

def test_merge_preserves_order
  a = {x: 1, y: 2}
  b = {y: 22, z: 3}
  result = a.merge(b)
  assert_equal [:x, :y, :z], result.keys
  assert_equal 22,            result[:y]
end

TESTS = [
  :test_keys_in_insertion_order,
  :test_each_in_insertion_order,
  :test_overwrite_does_not_reorder,
  :test_delete_then_reinsert_appends_at_end,
  :test_merge_preserves_order,
]
TESTS.each { |t| run_test(t) }
report "HashOrder"
