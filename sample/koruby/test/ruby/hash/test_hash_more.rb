require_relative "../../test_helper"

# Coverage for Hash methods missing or buggy in koruby before this commit:
# dig, value?/has_value?, group_by, sort_by, filter_map.

# ---------- Hash#dig ----------

def test_hash_dig_basic
  h = {a: {b: {c: 1}}}
  assert_equal 1, h.dig(:a, :b, :c)
end

def test_hash_dig_returns_nil_on_miss
  h = {a: {b: 1}}
  assert_equal nil, h.dig(:x)
  assert_equal nil, h.dig(:a, :x)
end

def test_hash_dig_into_array
  h = {list: [10, 20, 30]}
  assert_equal 20, h.dig(:list, 1)
end

def test_hash_dig_no_args_raises
  raised = false
  begin
    {}.dig
  rescue ArgumentError
    raised = true
  end
  assert raised, "Hash#dig with no args should raise ArgumentError"
end

# ---------- Hash#has_value? / value? ----------

def test_has_value_p
  h = {a: 1, b: 2}
  assert_equal true,  h.has_value?(1)
  assert_equal true,  h.has_value?(2)
  assert_equal false, h.has_value?(99)
end

def test_value_p_alias
  h = {a: 1}
  assert_equal true,  h.value?(1)
  assert_equal false, h.value?(2)
end

# ---------- Hash#group_by ----------

def test_group_by
  h = {a: 1, b: 2, c: 3, d: 1}
  grouped = h.group_by { |_k, v| v }
  assert_equal [[:a, 1], [:d, 1]], grouped[1]
  assert_equal [[:b, 2]],          grouped[2]
end

# ---------- Hash#sort_by ----------

def test_sort_by_returns_array_of_pairs
  h = {b: 2, a: 3, c: 1}
  result = h.sort_by { |_k, v| v }
  assert_equal [[:c, 1], [:b, 2], [:a, 3]], result
end

# ---------- Hash#filter_map ----------

def test_filter_map_drops_falsey
  h = {a: 1, b: 2, c: 3}
  result = h.filter_map { |_k, v| v * 10 if v.odd? }
  assert_equal [10, 30], result
end

TESTS = [
  :test_hash_dig_basic,
  :test_hash_dig_returns_nil_on_miss,
  :test_hash_dig_into_array,
  :test_hash_dig_no_args_raises,
  :test_has_value_p,
  :test_value_p_alias,
  :test_group_by,
  :test_sort_by_returns_array_of_pairs,
  :test_filter_map_drops_falsey,
]
TESTS.each { |t| run_test(t) }
report "HashMore"
