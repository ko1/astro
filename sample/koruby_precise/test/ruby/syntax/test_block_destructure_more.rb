require_relative "../../test_helper"

# Block parameter destructuring — koruby has a bug where the second
# positional param goes to nil if the first param is a destructure.

# ---------- Single destructure (this works today) ----------

def test_single_destructure
  result = proc { |(a, b)| [a, b] }.call([1, 2])
  assert_equal [1, 2], result
end

def test_array_each_destructure
  pairs = []
  [[1, 2], [3, 4]].each { |(a, b)| pairs << [a, b] }
  assert_equal [[1, 2], [3, 4]], pairs
end

# ---------- Destructure followed by another param ----------

def test_destructure_then_arg
  result = (proc { |(a, b), c| [a, b, c] }.call([1, 2], 3) rescue [:err, $!])
  assert_equal [1, 2, 3], result
end

def test_arg_then_destructure
  result = (proc { |a, (b, c)| [a, b, c] }.call(1, [2, 3]) rescue [:err, $!])
  assert_equal [1, 2, 3], result
end

def test_destructure_two_pairs
  result = (proc { |(a, b), (c, d)| [a, b, c, d] }.call([1, 2], [3, 4]) rescue [:err, $!])
  assert_equal [1, 2, 3, 4], result
end

# ---------- Destructure with leftover element ----------

def test_destructure_short_pad_with_nil
  # Destructuring more names than the array has → extras are nil.
  result = proc { |(a, b, c)| [a, b, c] }.call([1, 2])
  assert_equal [1, 2, nil], result
end

def test_destructure_long_drops_extras
  result = proc { |(a, b)| [a, b] }.call([1, 2, 3, 4])
  assert_equal [1, 2], result
end

# ---------- Hash#each_with_object with destructure (the optcarrot-shape) ----------

def test_hash_each_with_object_destructure
  out = ({a: 1, b: 2, c: 3}.each_with_object([]) { |(k, v), acc| acc << v if v.odd? } rescue [:err, $!])
  assert_equal [1, 3], out
end

def test_hash_each_destructure
  pairs = []
  {a: 1, b: 2}.each { |(k, v)| pairs << [k, v] }
  assert_equal [[:a, 1], [:b, 2]], pairs
end

TESTS = [
  :test_single_destructure,
  :test_array_each_destructure,
  :test_destructure_then_arg,
  :test_arg_then_destructure,
  :test_destructure_two_pairs,
  :test_destructure_short_pad_with_nil,
  :test_destructure_long_drops_extras,
  :test_hash_each_with_object_destructure,
  :test_hash_each_destructure,
]
TESTS.each { |t| run_test(t) }
report "BlockDestructureMore"
