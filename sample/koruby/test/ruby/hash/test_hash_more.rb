require_relative "../../test_helper"

# Additional Hash tests culled from CRuby's test_hash.rb.

def test_clear_h
  h = {a: 1, b: 2}
  h.clear
  assert_equal({}, h)
  assert_equal 0, h.size
end

def test_default
  h = Hash.new(0)
  assert_equal 0, h[:missing]
  h[:a] = 5
  assert_equal 5, h[:a]
end

def test_default_block
  h = Hash.new { |hash, k| k.to_s }
  assert_equal "missing", h[:missing]
end

def test_delete_if
  h = {a: 1, b: 2, c: 3}
  h.delete_if { |_, v| v.even? }
  assert_equal({a: 1, c: 3}, h)
end

def test_keep_if
  h = {a: 1, b: 2, c: 3}
  h.keep_if { |_, v| v.odd? }
  assert_equal({a: 1, c: 3}, h)
end

def test_compact_h
  h = {a: 1, b: nil, c: 3}
  assert_equal({a: 1, c: 3}, h.compact)
  assert_equal({a: 1, b: nil, c: 3}, h)  # non-destructive
end

def test_compact_bang
  h = {a: 1, b: nil, c: 3}
  h.compact!
  assert_equal({a: 1, c: 3}, h)
end

def test_clone_h
  h = {a: 1}
  h2 = h.clone
  h2[:b] = 2
  assert_equal({a: 1}, h)
end

def test_values_at
  h = {a: 1, b: 2, c: 3}
  assert_equal [1, 3, nil], h.values_at(:a, :c, :missing)
end

def test_fetch_values
  h = {a: 1, b: 2}
  assert_equal [1, 2], h.fetch_values(:a, :b)
end

def test_member_p
  h = {a: 1}
  assert_equal true, h.member?(:a)
  assert_equal false, h.member?(:b)
end

def test_reject_h
  h = {a: 1, b: 2, c: 3}
  r = h.reject { |_, v| v > 1 }
  assert_equal({a: 1}, r)
  assert_equal({a: 1, b: 2, c: 3}, h)
end

def test_select_h
  h = {a: 1, b: 2, c: 3}
  r = h.select { |_, v| v > 1 }
  assert_equal({b: 2, c: 3}, r)
end

def test_replace_h
  h = {a: 1}
  h.replace({x: 9, y: 8})
  assert_equal({x: 9, y: 8}, h)
end

def test_shift_h
  h = {a: 1, b: 2}
  pair = h.shift
  assert_equal [:a, 1], pair
  assert_equal({b: 2}, h)
end

def test_store
  h = {}
  h.store(:k, :v)
  assert_equal :v, h[:k]
end

def test_update
  h = {a: 1, b: 2}
  h.update({b: 99, c: 3})
  assert_equal({a: 1, b: 99, c: 3}, h)
end

def test_slice
  h = {a: 1, b: 2, c: 3}
  assert_equal({a: 1, c: 3}, h.slice(:a, :c, :missing))
end

def test_except
  h = {a: 1, b: 2, c: 3}
  assert_equal({a: 1, c: 3}, h.except(:b))
end

def test_size_alias_h
  h = {a: 1, b: 2}
  assert_equal 2, h.length
  assert_equal 2, h.size
  assert_equal 2, h.count
end

def test_to_a_h
  h = {a: 1, b: 2}
  assert_equal [[:a, 1], [:b, 2]], h.to_a
end

def test_min_max_by_h
  h = {a: 3, b: 1, c: 2}
  assert_equal [:b, 1], h.min_by { |_, v| v }
  assert_equal [:a, 3], h.max_by { |_, v| v }
end

def test_sort_h
  h = {b: 2, a: 1, c: 3}
  assert_equal [[:a, 1], [:b, 2], [:c, 3]], h.sort
end

def test_eq_h
  assert_equal true, {a: 1, b: 2} == {a: 1, b: 2}
  assert_equal true, {a: 1, b: 2} == {b: 2, a: 1}
  assert_equal false, {a: 1} == {a: 1, b: 2}
end

def test_inspect_h
  assert_equal "{}", {}.inspect
end

TESTS = [
  :test_clear_h, :test_default, :test_default_block,
  :test_delete_if, :test_keep_if,
  :test_compact_h, :test_compact_bang, :test_clone_h,
  :test_values_at, :test_fetch_values, :test_member_p,
  :test_reject_h, :test_select_h, :test_replace_h, :test_shift_h,
  :test_store, :test_update, :test_slice, :test_except,
  :test_size_alias_h, :test_to_a_h,
  :test_min_max_by_h, :test_sort_h,
  :test_eq_h, :test_inspect_h,
]
TESTS.each { |t| run_test(t) }
report "HashMore"
