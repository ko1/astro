require_relative "../../test_helper"

def test_basic
  h = {a: 1, b: 2}
  assert_equal 1, h[:a]
  assert_equal 2, h[:b]
  assert_equal nil, h[:c]
  assert_equal 2, h.size
  h[:c] = 3
  assert_equal 3, h[:c]
  assert_equal 3, h.size
end

def test_nested
  h = {a: {x: 1, y: 2}, b: {z: 3}}
  assert_equal 1, h[:a][:x]
  assert_equal 2, h[:a][:y]
  assert_equal 3, h[:b][:z]
  assert_equal [:x, :y], h[:a].keys
end

def test_keys_values
  h = {a: 1, b: 2, c: 3}
  assert_equal [:a, :b, :c], h.keys
  assert_equal [1, 2, 3], h.values
end

def test_each
  pairs = []
  {a: 1, b: 2}.each {|k, v| pairs << [k, v] }
  assert_equal [[:a, 1], [:b, 2]], pairs
end

def test_each_value
  vals = []
  {a: 1, b: 2}.each_value {|v| vals << v }
  assert_equal [1, 2], vals
end

def test_merge
  h = {a: 1, b: 2}
  m = h.merge(c: 3)
  assert_equal({a: 1, b: 2, c: 3}, m)
  # original untouched
  assert_equal 2, h.size
end

def test_merge_overwrite
  h = {a: 1, b: 2}.merge(b: 20, c: 3)
  assert_equal 20, h[:b]
  assert_equal 1, h[:a]
  assert_equal 3, h[:c]
end

def test_invert
  h = {a: 1, b: 2}.invert
  assert_equal :a, h[1]
  assert_equal :b, h[2]
end

def test_to_a
  assert_equal [[:a, 1], [:b, 2]], {a: 1, b: 2}.to_a
end

def test_delete
  h = {a: 1, b: 2}
  assert_equal 1, h.delete(:a)
  assert_equal 1, h.size
  assert_equal nil, h[:a]
  assert_equal nil, h.delete(:nope)
end

def test_key_p
  h = {a: 1}
  assert_equal true, h.key?(:a)
  assert_equal false, h.key?(:b)
  assert_equal true, h.has_key?(:a)
  assert_equal true, h.include?(:a)
end

def test_fetch
  h = {a: 1}
  assert_equal 1, h.fetch(:a)
  assert_equal "default", h.fetch(:b, "default")
  assert_equal :computed, h.fetch(:c) {|k| :computed }
end

def test_dup
  h = {a: 1}
  d = h.dup
  d[:b] = 2
  assert_equal 1, h.size
  assert_equal 2, d.size
end

def test_eq
  assert_equal true, {a: 1, b: 2} == {a: 1, b: 2}
  assert_equal false, {a: 1, b: 2} == {a: 1}
  assert_equal true, {a: {x: 1}} == {a: {x: 1}}  # nested
end

def test_string_keys
  h = {"foo" => 1, "bar" => 2}
  assert_equal 1, h["foo"]
  assert_equal 2, h["bar"]
end

def test_mixed_keys
  h = {1 => "one", :two => 2, "three" => :three}
  assert_equal "one", h[1]
  assert_equal 2, h[:two]
  assert_equal :three, h["three"]
end

def test_empty
  assert_equal true, {}.empty?
  assert_equal false, {a: 1}.empty?
end

# compare_by_identity returns self and switches the hash to identity-based
# key comparison.  optcarrot's PPU.setup_lut chains this:
#   @lut_update = {}.compare_by_identity
# A previous bug returned the inspect string instead of the hash, so the
# chain silently collapsed @lut_update to a String and attr_lut updates
# were dropped — caused PPU rendering to use stale TILE_LUT entries.
def test_compare_by_identity_returns_self
  h = {}
  result = h.compare_by_identity
  assert_equal h.object_id, result.object_id, "compare_by_identity must return self"
  assert_equal true, h.compare_by_identity?
end

# Even on a non-empty hash, calling compare_by_identity should work and
# return self (CRuby rehashes existing entries; koruby just keeps them).
def test_compare_by_identity_on_nonempty
  arr1 = [1, 2, 3]
  h = {}
  h[arr1] = "x"
  result = h.compare_by_identity
  assert_equal h.object_id, result.object_id
  # arr1 still resolvable by identity
  assert_equal "x", h[arr1]
end

# Hash with Array keys uses identity comparison — different Array instances
# with same content are different keys (this is how optcarrot keeps 4
# nmt_banks distinct in @lut_update).
def test_array_key_identity
  arr1 = [1, 2, 3]
  arr2 = [1, 2, 3]  # different object
  h = {}.compare_by_identity
  h[arr1] = "first"
  h[arr2] = "second"
  assert_equal 2, h.size
  assert_equal "first", h[arr1]
  assert_equal "second", h[arr2]
end

# Tests for Hash methods.


def test_transform_values
  result = {a: 1, b: 2}.transform_values { |v| v * 10 }
  assert_equal 10, result[:a]
  assert_equal 20, result[:b]
end

def test_transform_keys
  result = {a: 1, b: 2}.transform_keys { |k| k.to_s }
  assert_equal 1, result["a"]
  assert_equal 2, result["b"]
end

def test_hash_reject
  result = {a: 1, b: 2, c: 3}.reject { |k, v| v.even? }
  assert_equal 1, result[:a]
  assert_equal 3, result[:c]
  assert_equal nil, result[:b]
end

def test_hash_any
  assert_equal true, {a: 1, b: 2}.any? { |k, v| v == 2 }
  assert_equal false, {a: 1, b: 2}.any? { |k, v| v == 99 }
end

def test_hash_all
  assert_equal true, {a: 1, b: 2}.all? { |k, v| v > 0 }
  assert_equal false, {a: 1, b: 2}.all? { |k, v| v > 1 }
end

def test_hash_count
  assert_equal 3, {a: 1, b: 2, c: 3}.count
end

def test_hash_min_max_by
  assert_equal [:a, 1], {a: 1, b: 2, c: 3}.min_by { |k, v| v }
  assert_equal [:c, 3], {a: 1, b: 2, c: 3}.max_by { |k, v| v }
end

def test_hash_to_h
  h = {a: 1}
  assert_equal h, h.to_h
end

def test_hash_find
  pair = {a: 1, b: 2}.find { |k, v| v == 2 }
  assert_equal [:b, 2], pair
end

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

TESTS = %i[
  test_basic test_nested test_keys_values
  test_each test_each_value test_merge test_merge_overwrite
  test_invert test_to_a test_delete test_key_p
  test_fetch test_dup test_eq test_string_keys
  test_mixed_keys test_empty test_compare_by_identity_returns_self test_compare_by_identity_on_nonempty
  test_array_key_identity test_transform_values test_transform_keys test_hash_reject
  test_hash_any test_hash_all test_hash_count test_hash_min_max_by
  test_hash_to_h test_hash_find test_clear_h test_default
  test_default_block test_delete_if test_keep_if test_compact_h
  test_compact_bang test_clone_h test_values_at test_fetch_values
  test_member_p test_reject_h test_select_h test_replace_h
  test_shift_h test_store test_update test_slice
  test_except test_size_alias_h test_to_a_h test_min_max_by_h
  test_sort_h test_eq_h test_inspect_h
]
TESTS.each {|t| run_test(t) }
report "Hash"
