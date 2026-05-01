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

# Edge cases for division/modulo around zero and negatives
def test_div_mod_edges
  # 0 / n
  assert_equal 0, 0 / 5
  assert_equal 0, 0 % 5
  # n / 1
  assert_equal 7, 7 / 1
  assert_equal 0, 7 % 1
  # negative / positive — Ruby uses floor division
  assert_equal(-3, -7 / 3)        # math: -2.33 → floor = -3
  assert_equal 2, -7 % 3          # -7 = -3*3 + 2
  assert_equal(-3, 7 / -3)        # 7 / -3 = -2.33 → floor = -3
  assert_equal(-2, 7 % -3)        # 7 = -3*-3 + -2
  assert_equal 2, -7 / -3         # -7 / -3 = 2.33 → floor = 2
  assert_equal(-1, -7 % -3)       # -7 = 2*-3 + -1
  # divide-by-zero raises
  raised = false
  begin; 1 / 0; rescue; raised = true; end
  assert(raised, "1/0 should raise")
end

# Bit ops with negative numbers and shifts
def test_bitops_negative
  # negative AND-mask
  assert_equal 0xFE, -2 & 0xFF
  # left shift negative
  assert_equal(-256, -1 << 8)
  # right shift sign-extends
  assert_equal(-1, -1 >> 100)
  assert_equal(-128, -256 >> 1)
  # XOR with -1 = bitwise NOT
  assert_equal(-1, 0 ^ -1)
end

# FIXNUM boundary into Bignum and back
def test_fixnum_boundary
  fmax = (1 << 62) - 1
  fmin = -(1 << 62)
  assert_equal (1 << 62), fmax + 1
  assert_equal -(1 << 62) - 1, fmin - 1
  # Multiplication overflow
  assert_equal (1 << 62) * 4, 4611686018427387904 * 4
  # Bignum subtract back to Fixnum
  big = 1 << 62
  result = big - 1
  assert_equal fmax, result
end

# Comparison: Fixnum vs Float
def test_compare_int_float
  assert_equal true, 1 == 1.0
  assert_equal true, 1 < 1.5
  assert_equal false, 2 < 1.5
  assert_equal -1, (1 <=> 1.5)
  assert_equal 1, (2 <=> 1.5)
  assert_equal 0, (1 <=> 1.0)
end

# Integer methods
def test_int_methods
  assert_equal 5,   (-5).abs
  assert_equal 5,   5.abs
  assert_equal 0,   0.abs
  assert_equal true, 5.positive?
  assert_equal false, 0.positive?
  assert_equal true, (-1).negative?
  assert_equal false, 0.negative?
  assert_equal true, 0.zero?
  assert_equal false, 1.zero?
end

# Even/odd
def test_even_odd
  assert_equal true, 0.even?
  assert_equal false, 0.odd?
  assert_equal true, 1.odd?
  assert_equal false, 1.even?
  assert_equal true, (-2).even?
  assert_equal true, (-3).odd?
end

# Power (**)
def test_pow
  assert_equal 1024, 2 ** 10
  assert_equal 1, 5 ** 0
  assert_equal 5, 5 ** 1
  assert_equal 1, 0 ** 0    # Ruby quirk
  assert_equal 0, 0 ** 5
  assert_equal -8, (-2) ** 3
  assert_equal 4, (-2) ** 2
end

# Range conversion
def test_to_s_corner
  assert_equal "0", 0.to_s
  assert_equal "-1", (-1).to_s
  assert_equal "0", 0.to_s(2)
  assert_equal "1010", 10.to_s(2)
  assert_equal "ff", 255.to_s(16)
  assert_equal "FF", 255.to_s(16).upcase
  # negative in different bases
  assert_equal "-ff", (-255).to_s(16)
end

# Range iteration
def test_step_with_block_break
  arr = []
  result = 0.step(100, 1) do |x|
    arr << x
    break :done if x == 5
  end
  assert_equal [0, 1, 2, 3, 4, 5], arr
end

# Comparable mixed-type fallback
def test_spaceship_returns_nil
  # Integer <=> non-numeric should return nil
  assert_equal nil, (1 <=> "x")
end

TESTS = %i[
  test_basic test_compare test_bitops test_bit_access
  test_bignum test_divmod test_times test_step test_upto_downto
  test_float_coerce test_chr test_to_s_base
  test_div_mod_edges test_bitops_negative test_fixnum_boundary
  test_compare_int_float test_int_methods test_even_odd
  test_pow test_to_s_corner test_step_with_block_break
  test_spaceship_returns_nil
]

TESTS.each {|t| run_test(t) }
report("Integer")
