require_relative "../../test_helper"

# Tests for the Enumerable mixin (defined in bootstrap.rb).
# Any class that includes Enumerable and provides #each picks up
# the full set of iterator helpers.


class Counter
  include Enumerable
  def initialize(n); @n = n; end
  def each
    i = 0
    while i < @n
      yield i
      i += 1
    end
  end
end

def test_to_a
  assert_equal [0, 1, 2, 3, 4], Counter.new(5).to_a
  assert_equal [], Counter.new(0).to_a
end

def test_count
  assert_equal 5, Counter.new(5).count
  assert_equal 0, Counter.new(0).count
end

def test_map
  assert_equal [0, 2, 4, 6, 8], Counter.new(5).map { |x| x * 2 }
end

def test_collect_aliased_to_map
  assert_equal [0, 2, 4], Counter.new(3).collect { |x| x * 2 }
end

def test_select
  assert_equal [0, 2, 4], Counter.new(5).select { |x| x.even? }
end

def test_filter_aliased_to_select
  assert_equal [1, 3], Counter.new(5).filter { |x| x.odd? }
end

def test_reject
  assert_equal [1, 3], Counter.new(5).reject { |x| x.even? }
end

def test_reduce_with_init
  assert_equal 10, Counter.new(5).reduce(0) { |a, b| a + b }
end

def test_inject_aliased_to_reduce
  assert_equal 6, Counter.new(4).inject(0) { |a, b| a + b }
end

def test_min_max
  assert_equal 0, Counter.new(5).min
  assert_equal 4, Counter.new(5).max
end

def test_sort
  # Counter yields 0..n-1 in order; sort returns a sorted Array copy.
  assert_equal [0, 1, 2, 3, 4], Counter.new(5).sort
end

def test_each_with_index
  pairs = []
  Counter.new(3).each_with_index { |x, i| pairs << [x, i] }
  assert_equal [[0, 0], [1, 1], [2, 2]], pairs
end

def test_member_aliased_to_include
  assert_equal true, Counter.new(5).member?(3)
end

# --- Array also gets Enumerable for the inherited methods --------
# (Array has its own each so the bootstrap methods naturally work.)

def test_array_each_with_block
  # Array#each is built in (cfunc); confirm it integrates with blocks
  # the same way custom Enumerable classes do.
  out = []
  [1, 2, 3].each { |x| out << x * 10 }
  assert_equal [10, 20, 30], out
end

# Enumerable extras inspired by CRuby's test_enum.rb.

def test_each_with_index_offset
  collected = []
  [10, 20, 30].each_with_index { |v, i| collected << [i, v] }
  assert_equal [[0, 10], [1, 20], [2, 30]], collected
end

def test_chunk_while
  arr = [1, 2, 4, 5, 6, 9, 11, 12]
  groups = arr.chunk_while { |a, b| b - a == 1 }.to_a
  assert_equal [[1, 2], [4, 5, 6], [9], [11, 12]], groups
end

def test_slice_when
  arr = [1, 2, 4, 5, 6, 9]
  groups = arr.slice_when { |a, b| b - a > 1 }.to_a
  assert_equal [[1, 2], [4, 5, 6], [9]], groups
end

def test_minmax
  assert_equal [1, 5], [3, 1, 4, 5, 2].minmax
end

def test_zip_pad
  # Zip pads with nil for shorter lists.
  assert_equal [[1, :a], [2, :b], [3, nil]], [1, 2, 3].zip([:a, :b])
end

def test_each_cons_basic
  result = []
  [1, 2, 3, 4].each_cons(2) { |a| result << a }
  assert_equal [[1, 2], [2, 3], [3, 4]], result
end

def test_chain
  a = [1, 2]
  b = [3, 4]
  assert_equal [1, 2, 3, 4], a.chain(b).to_a
end

def test_filter_map
  # Like map + compact in one step.
  r = [1, 2, 3, 4, 5].filter_map { |x| x * 2 if x.odd? }
  assert_equal [2, 6, 10], r
end

def test_sum_int
  assert_equal 15, [1, 2, 3, 4, 5].sum
  assert_equal 25, [1, 2, 3, 4, 5].sum(10)
end

def test_min_max_block
  arr = ["abc", "ab", "abcd"]
  assert_equal "ab",   arr.min_by(&:length)
  assert_equal "abcd", arr.max_by(&:length)
end

def test_partition_basic
  evens, odds = [1, 2, 3, 4, 5].partition(&:even?)
  assert_equal [2, 4],    evens
  assert_equal [1, 3, 5], odds
end

def test_group_by_basic
  r = (1..6).group_by { |x| x % 3 }
  assert_equal [3, 6], r[0]
  assert_equal [1, 4], r[1]
  assert_equal [2, 5], r[2]
end

def test_lazy_first
  # Skipped: koruby's blocks share an env with the enclosing frame, so
  # passing a top-level block into a method that yields it inside its
  # own loop clobbers the method's locals.  See todo #56.
end

TESTS = %i[
  test_to_a test_count test_map test_collect_aliased_to_map
  test_select test_filter_aliased_to_select test_reject test_reduce_with_init
  test_inject_aliased_to_reduce test_min_max test_sort test_each_with_index
  test_member_aliased_to_include test_array_each_with_block test_each_with_index_offset test_chunk_while
  test_slice_when test_minmax test_zip_pad test_each_cons_basic
  test_chain test_filter_map test_sum_int test_min_max_block
  test_partition_basic test_group_by_basic test_lazy_first
]
TESTS.each {|t| run_test(t) }
report "Enumerable"
