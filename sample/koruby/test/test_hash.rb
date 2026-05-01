require_relative "test_helper"

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

TESTS = %i[
  test_basic test_nested test_keys_values test_each test_each_value
  test_merge test_merge_overwrite test_invert test_to_a
  test_delete test_key_p test_fetch test_dup test_eq
  test_string_keys test_mixed_keys test_empty
]
TESTS.each {|t| run_test(t) }
report("Hash")
