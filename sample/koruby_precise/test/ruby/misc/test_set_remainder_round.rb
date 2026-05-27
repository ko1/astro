require_relative "../../test_helper"

# Set (Hash-backed), Integer#remainder, Integer#round/floor/ceil with neg ndigits.

# ---------- Set ----------

def test_set_basic
  s = Set.new([1, 2, 3])
  assert_equal true,  s.include?(2)
  assert_equal false, s.include?(99)
  assert_equal 3, s.size
end

def test_set_add_delete
  s = Set.new
  s << 1
  s.add(2)
  assert_equal true,  s.include?(1)
  s.delete(1)
  assert_equal false, s.include?(1)
  assert_equal true,  s.include?(2)
end

def test_set_union_intersection_difference
  a = Set.new([1, 2, 3])
  b = Set.new([2, 3, 4])
  assert_equal [1, 2, 3, 4], (a | b).to_a.sort
  assert_equal [2, 3],       (a & b).to_a.sort
  assert_equal [1],          (a - b).to_a.sort
end

def test_set_equality
  assert Set.new([1, 2, 3]) == Set.new([3, 2, 1])
  refute_equal = !(Set.new([1, 2]) == Set.new([1, 3]))
  assert refute_equal
end

def test_set_subset
  a = Set.new([1, 2])
  b = Set.new([1, 2, 3])
  assert a.subset?(b)
  refute_b_in_a = !b.subset?(a)
  assert refute_b_in_a
end

# ---------- Integer#remainder ----------

def test_remainder_truncates_toward_zero
  assert_equal 1,  10.remainder(3)
  assert_equal(-1, (-10).remainder(3))    # truncating, not flooring
  assert_equal 2,  (-10) % 3              # modulo (floor) for contrast
end

# ---------- Integer round/floor/ceil with negative precision ----------

def test_int_round_negative_precision
  assert_equal 150, 154.round(-1)
  assert_equal 160, 156.round(-1)
  assert_equal 100, 100.round(-2)
end

def test_int_round_negative
  assert_equal(-150, (-154).round(-1))
  assert_equal(-160, (-156).round(-1))
end

def test_int_floor_negative_precision
  assert_equal 150, 159.floor(-1)
  assert_equal(-160, (-154).floor(-1))    # floor toward -inf
end

def test_int_ceil_negative_precision
  assert_equal 160, 154.ceil(-1)
  assert_equal(-150, (-154).ceil(-1))     # ceil toward +inf
end

def test_int_round_zero_or_pos_is_identity
  assert_equal 5, 5.round
  assert_equal 5, 5.round(2)
end

TESTS = [
  :test_set_basic,
  :test_set_add_delete,
  :test_set_union_intersection_difference,
  :test_set_equality,
  :test_set_subset,
  :test_remainder_truncates_toward_zero,
  :test_int_round_negative_precision,
  :test_int_round_negative,
  :test_int_floor_negative_precision,
  :test_int_ceil_negative_precision,
  :test_int_round_zero_or_pos_is_identity,
]
TESTS.each { |t| run_test(t) }
report "SetRemainderRound"
