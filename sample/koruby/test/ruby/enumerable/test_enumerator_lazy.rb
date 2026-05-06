require_relative "../../test_helper"

# Enumerator::Lazy — chained transformations that materialize lazily.

def infinite
  Enumerator.new do |y|
    i = 0
    loop { y << i; i += 1 }
  end
end

def test_lazy_returns_lazy
  l = infinite.lazy
  assert(l.is_a?(Enumerator::Lazy))
  assert(l.is_a?(Enumerator))
end

def test_lazy_map_select_chain
  result = infinite.lazy
                   .map { |x| x * 2 }
                   .select { |x| x % 3 == 0 }
                   .first(5)
  assert_equal [0, 6, 12, 18, 24], result
end

def test_lazy_take
  result = infinite.lazy.take(5).to_a
  assert_equal [0, 1, 2, 3, 4], result
end

def test_lazy_take_while
  result = infinite.lazy.take_while { |x| x < 5 }.to_a
  assert_equal [0, 1, 2, 3, 4], result
end

def test_lazy_drop
  e = Enumerator.new { |y| 10.times { |i| y << i } }
  result = e.lazy.drop(7).to_a
  assert_equal [7, 8, 9], result
end

def test_lazy_drop_while
  e = Enumerator.new { |y| 10.times { |i| y << i } }
  result = e.lazy.drop_while { |x| x < 4 }.to_a
  assert_equal [4, 5, 6, 7, 8, 9], result
end

def test_lazy_reject
  e = Enumerator.new { |y| 6.times { |i| y << i } }
  result = e.lazy.reject { |x| x.even? }.to_a
  assert_equal [1, 3, 5], result
end

def test_lazy_with_index
  e = Enumerator.new { |y| %i[a b c].each { |s| y << s } }
  result = e.lazy.with_index.to_a
  assert_equal [[:a, 0], [:b, 1], [:c, 2]], result
end

def test_lazy_with_index_offset
  e = Enumerator.new { |y| %i[a b].each { |s| y << s } }
  result = e.lazy.with_index(10).to_a
  assert_equal [[:a, 10], [:b, 11]], result
end

def test_lazy_force_alias
  e = Enumerator.new { |y| 3.times { |i| y << i } }
  assert_equal [0, 1, 2], e.lazy.force
  assert_equal [0, 1, 2], e.lazy.to_a
end

def test_lazy_chain_does_not_materialize_early
  # If the lazy chain materialized eagerly, a chain over an infinite
  # source would hang here.  Verify it terminates.
  result = infinite.lazy.map { |x| x }.select { |x| x > 100 }.first(2)
  assert_equal [101, 102], result
end

def test_range_lazy_enum_bridge
  result = (1..Float::INFINITY).lazy_enum.map { |x| x * x }.first(5)
  assert_equal [1, 4, 9, 16, 25], result
end

def test_array_lazy_enum_bridge
  result = [1, 2, 3, 4, 5].lazy_enum.select { |x| x.even? }.to_a
  assert_equal [2, 4], result
end

TESTS = %i[
  test_lazy_returns_lazy test_lazy_map_select_chain
  test_lazy_take test_lazy_take_while
  test_lazy_drop test_lazy_drop_while
  test_lazy_reject test_lazy_with_index test_lazy_with_index_offset
  test_lazy_force_alias test_lazy_chain_does_not_materialize_early
  test_range_lazy_enum_bridge test_array_lazy_enum_bridge
]
TESTS.each {|t| run_test(t) }
report "EnumeratorLazy"
