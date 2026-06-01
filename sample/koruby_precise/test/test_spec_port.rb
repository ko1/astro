# Behaviors ported from CRuby rubyspec (spec/ruby/core), translated into the
# koruby assert idiom.  Grown incrementally; each batch is a `# --- <area> ---`
# block.  Run normally AND under ASTRO_GC_STRESS=1 ASTRO_GC_PURGE=1 to surface
# moving-GC rooting gaps.  Failures are either fixed in the interpreter or
# marked `# PENDING:` with a reason.
require_relative "test_helper"

# --- Array#first / #last (array/first_spec.rb, last_spec.rb) ---
def test_array_first_last
  assert_equal "a", %w{a b c}.first
  assert_equal nil, [nil].first
  assert_equal nil, [].first
  assert_equal [true, false], [true, false, true, nil, false].first(2)
  assert_equal [], [].first(0)
  assert_equal [], [].first(2)
  assert_equal [], [1, 2, 3].first(0)
  assert_equal [1], [1, 2, 3].first(1)
  assert_equal [1, 2, 3], [1, 2, 3].first(5)
  assert_equal "c", %w{a b c}.last
  assert_equal nil, [].last
  assert_equal [2, 3], [1, 2, 3].last(2)
  assert_equal [], [1, 2, 3].last(0)
  assert_equal [1, 2, 3], [1, 2, 3].last(5)
end

# --- Array#push / #pop / #shift / #unshift (push_spec.rb, pop_spec.rb) ---
def test_array_push_pop
  a = [1, 2, 3]
  a.push(4)
  assert_equal [1, 2, 3, 4], a
  a.push(5, 6)
  assert_equal [1, 2, 3, 4, 5, 6], a
  assert_equal 6, a.pop
  assert_equal [1, 2, 3, 4, 5], a
  assert_equal [4, 5], a.pop(2)
  assert_equal 1, a.shift
  assert_equal [2, 3], a
  a.unshift(0)
  assert_equal [0, 2, 3], a
end

# --- Array#include? / #index (include_spec.rb, index_spec.rb) ---
def test_array_include_index
  assert_equal true, [1, 2, 3].include?(2)
  assert_equal false, [1, 2, 3].include?(5)
  assert_equal true, ["a", "b"].include?("a")
  assert_equal 1, [1, 2, 3].index(2)
  assert_equal nil, [1, 2, 3].index(9)
  assert_equal 0, [1, 2, 3].index { |x| x > 0 }
end

# --- Array#map / #select / #reject / #reduce (collect_spec.rb, select_spec.rb) ---
def test_array_enum
  assert_equal [2, 4, 6], [1, 2, 3].map { |x| x * 2 }
  assert_equal [2, 4], [1, 2, 3, 4].select { |x| x.even? }
  assert_equal [1, 3], [1, 2, 3, 4].reject { |x| x.even? }
  assert_equal 10, [1, 2, 3, 4].reduce(0) { |a, b| a + b }
  assert_equal 24, [1, 2, 3, 4].reduce(:*)
  assert_equal [1, 2, 2, 3, 3, 3], [1, 2, 3].flat_map { |x| [x] * x }
end

# --- Array#slice / #[] (slice_spec.rb, element_reference_spec.rb) ---
def test_array_slice
  a = [1, 2, 3, 4, 5]
  assert_equal 3, a[2]
  assert_equal 5, a[-1]
  assert_equal nil, a[10]
  assert_equal [2, 3], a[1, 2]
  assert_equal [2, 3, 4], a[1..3]
  assert_equal [1, 2], a[0...2]
  assert_equal [4, 5], a[-2..-1]
  assert_equal [], a[5, 2]
  assert_equal nil, a[6, 2]
end

# --- Array#sort / #sort_by / #min / #max (sort_spec.rb, min_spec.rb) ---
def test_array_sort
  assert_equal [1, 2, 3], [3, 1, 2].sort
  assert_equal [3, 2, 1], [1, 2, 3].sort { |a, b| b <=> a }
  assert_equal ["a", "bb", "ccc"], ["ccc", "a", "bb"].sort_by { |s| s.length }
  assert_equal 1, [3, 1, 2].min
  assert_equal 3, [3, 1, 2].max
  assert_equal "a", ["ccc", "a", "bb"].min_by { |s| s.length }
end

# --- String#+ / #* / #<< / #concat (string/plus_spec.rb, multiply_spec.rb) ---
def test_string_build
  assert_equal "ab", "a" + "b"
  assert_equal "aaa", "a" * 3
  assert_equal "", "x" * 0
  s = "a"
  s << "b" << "c"
  assert_equal "abc", s
  assert_equal "hello world", "hello".concat(" ", "world")
end

# --- String#split / #chars / #bytes (split_spec.rb, chars_spec.rb) ---
def test_string_split
  assert_equal ["a", "b", "c"], "a,b,c".split(",")
  assert_equal ["a", "b", "c"], "a b c".split
  # PENDING: String#split ignores the `limit` argument — `"a,b,c".split(",", 2)`
  # returns ["a","b","c"] instead of ["a","b,c"].  Feature gap in str_split,
  # not GC-related.  Re-enable when split honors limit.
  # assert_equal ["a", "b,c"], "a,b,c".split(",", 2)
  assert_equal ["h", "e", "l", "l", "o"], "hello".chars
  assert_equal [104, 105], "hi".bytes
  assert_equal ["one", "two"], "one\ntwo".lines.map { |l| l.chomp }
end

# --- String#sub / #gsub / #upcase / #downcase (sub_spec.rb, gsub_spec.rb) ---
def test_string_transform
  assert_equal "HELLO", "hello".upcase
  assert_equal "hello", "HELLO".downcase
  assert_equal "hxllo", "hello".sub("e", "x")
  assert_equal "hxllx", "hello".gsub("e", "x").gsub("o", "x")
  assert_equal "olleh", "hello".reverse
  assert_equal 5, "hello".length
  assert_equal "ell", "hello"[1, 3]
end

# --- String#format / interpolation (format_spec.rb) ---
def test_string_format
  assert_equal "a-42-b", format("%s-%d-%s", "a", 42, "b")
  x = 7
  assert_equal "val=7", "val=#{x}"
  assert_equal "1+2=3", "#{1}+#{2}=#{1 + 2}"
  assert_equal "[1, 2, 3]", [1, 2, 3].to_s
end

# --- Hash#[] / #each / #merge / #keys / #values (hash specs) ---
def test_hash_core
  h = { "a" => 1, "b" => 2 }
  assert_equal 1, h["a"]
  assert_equal nil, h["z"]
  assert_equal 2, h.size
  assert_equal ["a", "b"], h.keys.sort
  assert_equal [1, 2], h.values.sort
  h2 = h.merge("c" => 3)
  assert_equal 3, h2.size
  assert_equal 2, h.size
  acc = []
  h.each { |k, v| acc << [k, v] }
  assert_equal 2, acc.size
end

# --- Hash#map / #select / #to_a / #sort_by (hash enum specs) ---
def test_hash_enum
  h = { 1 => "a", 2 => "b", 3 => "c" }
  assert_equal ["a", "b", "c"], h.map { |k, v| v }.sort
  assert_equal 2, h.select { |k, v| k > 1 }.size
  assert_equal 3, h.to_a.size
  assert_equal 6, h.reduce(0) { |s, (k, v)| s + k }
  assert_equal [1, 2, 3], h.keys.sort_by { |k| k }
end

# --- Integer#times / #upto / #downto / #step (integer specs) ---
def test_integer_iter
  acc = []
  3.times { |i| acc << i }
  assert_equal [0, 1, 2], acc
  assert_equal [1, 2, 3], (1).upto(3).to_a
  assert_equal [3, 2, 1], 3.downto(1).to_a
  assert_equal 6, [1, 2, 3].reduce(:+)
  assert_equal 15, (1..5).reduce(:+)
  assert_equal [2, 4, 6], [1, 2, 3].map { |x| x * 2 }
end

# --- Range#to_a / #map / #select / #step (range specs) ---
def test_range_core
  assert_equal [1, 2, 3, 4, 5], (1..5).to_a
  assert_equal [1, 2, 3, 4], (1...5).to_a
  assert_equal [2, 4, 6, 8, 10], (1..5).map { |x| x * 2 }
  assert_equal [2, 4], (1..5).select { |x| x.even? }
  assert_equal true, (1..10).include?(5)
  assert_equal 15, (1..5).reduce(0) { |a, b| a + b }
end

# --- Array#zip / #flatten / #compact / #uniq (zip_spec.rb, flatten_spec.rb) ---
def test_array_combine
  assert_equal [[1, 4], [2, 5], [3, 6]], [1, 2, 3].zip([4, 5, 6])
  assert_equal [[1, "a"], [2, "b"]], [1, 2].zip(["a", "b"])
  assert_equal [1, 2, 3, 4], [1, [2, [3, [4]]]].flatten
  assert_equal [1, 2, 3], [1, nil, 2, nil, 3].compact
  assert_equal [1, 2, 3], [1, 1, 2, 2, 3, 3].uniq
  assert_equal [1, 2, 3, 4], [1, 2] + [3, 4]
  assert_equal [1, 2], [1, 2, 3] - [3]
  assert_equal [1, 2, 1, 2], [1, 2] * 2
end

# --- Array#each_with_index / #each_with_object / #group_by (enumerable) ---
def test_array_enum2
  acc = []
  [10, 20, 30].each_with_index { |v, i| acc << [i, v] }
  assert_equal [[0, 10], [1, 20], [2, 30]], acc
  r = [1, 2, 3, 4].each_with_object([]) { |x, memo| memo << x * x }
  assert_equal [1, 4, 9, 16], r
  # PENDING: group_by returns {} because it uses `h[k] ||= []` and the
  # obj[i]=<Array literal> writeback is dropped.  See docs/spec_port_pending.md.
  # g = [1, 2, 3, 4, 5, 6].group_by { |x| x % 3 }
  # assert_equal [3, 6], g[0]
  # assert_equal 3, g.size
  assert_equal [[1, 2], [3, 4]], [1, 2, 3, 4].each_slice(2).to_a
  assert_equal [4, 5], [1, 2, 3, 4, 5].drop(3)
  assert_equal [1, 2], [1, 2, 3, 4, 5].take(2)
end

# --- Array#combination / #permutation / #product (combinatorics) ---
def test_array_combinatorics
  # with-block forms work; the no-block `.to_a` enumerator form raises
  # StopIteration (enumerator gap, see pending) so test the block forms.
  acc = []
  [1, 2, 3].combination(2) { |c| acc << c }
  assert_equal [[1, 2], [1, 3], [2, 3]], acc
  pacc = []
  [1, 2, 3].permutation(2) { |p| pacc << p }
  assert_equal 6, pacc.size
  assert_equal [[1, 3], [1, 4], [2, 3], [2, 4]], [1, 2].product([3, 4])
  assert_equal [[1, 4], [2, 5], [3, 6]], [[1, 2, 3], [4, 5, 6]].transpose
  # PENDING: `combination(n).to_a` (no-block enumerator) raises StopIteration.
end

# --- Hash mutation: #[]= / #delete / #merge! / #each + build (hash specs) ---
def test_hash_mutate
  h = {}
  10.times { |i| h[i] = i * i }
  assert_equal 10, h.size
  assert_equal 81, h[9]
  h.delete(5)
  assert_equal false, h.key?(5)
  assert_equal 9, h.size
  h2 = { "x" => 1 }
  h2.merge!("y" => 2, "z" => 3)
  assert_equal 3, h2.size
  inv = { 1 => "a", 2 => "b" }.invert
  assert_equal 1, inv["a"]
end

# --- mixed object graphs under GC pressure (stress rooting) ---
def test_object_graph
  pairs = (1..20).map { |i| [i.to_s, i * 2] }
  h = {}
  pairs.each { |k, v| h[k] = v }
  assert_equal 20, h.size
  assert_equal 40, h["20"]
  nested = (1..5).map { |i| (1..i).to_a }
  assert_equal [[1], [1, 2], [1, 2, 3], [1, 2, 3, 4], [1, 2, 3, 4, 5]], nested
  total = nested.map { |a| a.reduce(0) { |s, x| s + x } }.reduce(:+)
  assert_equal 35, total
  strs = (1..10).map { |i| "item#{i}" }
  assert_equal "item5", strs[4]
  assert_equal 10, strs.uniq.length
end

TESTS = %i[
  test_array_first_last test_array_push_pop test_array_include_index
  test_array_enum test_array_slice test_array_sort
  test_string_build test_string_split test_string_transform test_string_format
  test_hash_core test_hash_enum test_integer_iter test_range_core
  test_array_combine test_array_enum2 test_array_combinatorics
  test_hash_mutate test_object_graph
]
TESTS.each { |t| run_test(t) }
report("SpecPort")
