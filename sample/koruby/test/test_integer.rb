require_relative "test_helper"

# Basic arithmetic
def test_basic
  assert_equal 3, 1 + 2
  assert_equal -1, 1 - 2
  assert_equal 6, 2 * 3
  assert_equal 2, 7 / 3
  assert_equal -3, -7 / 3, "Ruby uses floor division"
  assert_equal 1, 7 % 3
  assert_equal 2, -7 % 3, "modulo follows divisor sign in Ruby"
end

# Comparison
def test_compare
  assert_equal true,  1 < 2
  assert_equal false, 2 < 1
  assert_equal true,  1 <= 1
  assert_equal true,  1 == 1
  assert_equal false, 1 == 2
  assert_equal -1, (1 <=> 2)
  assert_equal 0,  (1 <=> 1)
  assert_equal 1,  (2 <=> 1)
end

# Bit ops
def test_bitops
  assert_equal 0xF0, 0xFF & 0xF0
  assert_equal 0xFF, 0xF0 | 0x0F
  assert_equal 0xFF, 0xF0 ^ 0x0F
  assert_equal 0x100, 1 << 8
  assert_equal 0x01, 0x100 >> 8
  assert_equal -1, ~0
  assert_equal 0xFFFF, ~0 & 0xFFFF
  # signed right shift
  assert_equal -1, -1 >> 8
  # mask with negative
  assert_equal 0xFFFF, (-1) & 0xFFFF
end

# Integer#[bit] for bit access
def test_bit_access
  assert_equal 1, 0b1010[1]
  assert_equal 0, 0b1010[0]
  assert_equal 1, 0xFF[7]
  assert_equal 0, 0[0]
end

# Overflow into Bignum
def test_bignum
  big = 1 << 62
  assert_equal 2, big * 2 / big
  # FIXNUM_MAX = (1<<62)-1 on x86_64 koruby; +1 should still be fine
  v = (1 << 62) - 1
  v2 = v + 1
  assert_equal (1 << 62), v2
end

# divmod
def test_divmod
  q, r = 17.divmod(5)
  assert_equal 3, q
  assert_equal 2, r
  q, r = (-17).divmod(5)
  assert_equal(-4, q)
  assert_equal 3, r
end

# Iteration
def test_times
  total = 0
  5.times {|i| total += i }
  assert_equal 10, total
end

def test_step
  arr = []
  0.step(10, 2) {|x| arr << x }
  assert_equal [0, 2, 4, 6, 8, 10], arr
end

def test_upto_downto
  arr = []
  3.upto(7) {|x| arr << x }
  assert_equal [3, 4, 5, 6, 7], arr
  arr = []
  5.downto(2) {|x| arr << x }
  assert_equal [5, 4, 3, 2], arr
end

# Float coercion
def test_float_coerce
  assert_equal 3.5, 1 + 2.5
  assert_equal 0.5, 1 - 0.5
  assert_equal 3.0, 1.5 * 2
  assert_equal 0.5, 1 / 2.0
end

# chr / ord
def test_chr
  assert_equal "A", 65.chr
  assert_equal "0", 48.chr
  assert_equal 32, " ".bytes[0]
end

# to_s with base
def test_to_s_base
  assert_equal "ff", 255.to_s(16)
  assert_equal "11111111", 255.to_s(2)
  assert_equal "377", 255.to_s(8)
end

TESTS = %i[
  test_basic test_compare test_bitops test_bit_access
  test_bignum test_divmod test_times test_step test_upto_downto
  test_float_coerce test_chr test_to_s_base
]

TESTS.each {|t| run_test(t) }
report("Integer")
