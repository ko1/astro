require_relative "../../test_helper"

# Random class, Kernel#rand / srand, Bignum#to_f, Array#sample(n).

# ---------- Kernel#rand ----------

def test_rand_no_arg_in_unit_interval
  v = rand
  assert v.is_a?(Float)
  assert v >= 0.0 && v < 1.0
end

def test_rand_integer_arg
  v = rand(10)
  assert v.is_a?(Integer)
  assert v >= 0 && v < 10
end

def test_rand_range
  v = rand(1..6)
  assert v >= 1 && v <= 6
end

def test_srand_returns_seed
  seed = srand(12345)
  # CRuby returns the previous seed; we accept any integer.
  assert seed.is_a?(Integer)
end

# ---------- Random class ----------

def test_random_with_seed_reproducible
  r1 = Random.new(42)
  r2 = Random.new(42)
  3.times do
    assert_equal r1.rand, r2.rand
  end
end

def test_random_rand_in_unit_interval
  r = Random.new(7)
  v = r.rand
  assert v.is_a?(Float)
  assert v >= 0.0 && v < 1.0
end

def test_random_rand_integer
  r = Random.new(7)
  v = r.rand(100)
  assert v.is_a?(Integer)
  assert v >= 0 && v < 100
end

def test_random_rand_range
  r = Random.new(7)
  vals = 10.times.map { r.rand(1..6) }
  vals.each { |v| assert v >= 1 && v <= 6 }
end

# ---------- Bignum#to_f ----------

def test_bignum_to_f_roughly_correct
  big = 10 ** 18
  f = big.to_f
  # 10**18 in double is exact-ish; should be ~ 1e18.
  assert (f - 1.0e18).abs < 1.0e2
end

def test_bignum_huge_to_f_finite
  big = 2 ** 62
  f = big.to_f
  assert f > 4.0e18
  assert f.finite?
end

# ---------- Array#sample(n) ----------

def test_array_sample_no_arg_returns_element
  arr = [1, 2, 3, 4, 5]
  assert arr.include?(arr.sample)
end

def test_array_sample_n
  arr = [1, 2, 3, 4, 5]
  s = arr.sample(3)
  assert_equal 3, s.length
  s.each { |v| assert arr.include?(v) }
  assert_equal s.uniq.size, s.size  # no replacement
end

def test_array_sample_n_larger_than_array
  arr = [1, 2, 3]
  s = arr.sample(10)
  assert_equal 3, s.length
  assert_equal s.sort, arr.sort
end

TESTS = [
  :test_rand_no_arg_in_unit_interval,
  :test_rand_integer_arg,
  :test_rand_range,
  :test_srand_returns_seed,
  :test_random_with_seed_reproducible,
  :test_random_rand_in_unit_interval,
  :test_random_rand_integer,
  :test_random_rand_range,
  :test_bignum_to_f_roughly_correct,
  :test_bignum_huge_to_f_finite,
  :test_array_sample_no_arg_returns_element,
  :test_array_sample_n,
  :test_array_sample_n_larger_than_array,
]
TESTS.each { |t| run_test(t) }
report "RandomIOExtras"
