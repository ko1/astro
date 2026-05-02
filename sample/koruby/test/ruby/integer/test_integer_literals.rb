require_relative "../../test_helper"

# Integer literal forms: 0b, 0o, 0x, underscore, negative.

def test_decimal
  assert_equal 0,          0
  assert_equal 1234567890, 1_234_567_890
  assert_equal -42,        -42
end

def test_binary_literal
  assert_equal 5, 0b101
  assert_equal 8, 0b1000
  assert_equal 0, 0b0
end

def test_octal_literal
  assert_equal 8,  0o10
  assert_equal 15, 0o17
  assert_equal 8,  010      # leading-0 = octal
end

def test_hex_literal
  assert_equal 255, 0xff
  assert_equal 16,  0x10
  assert_equal 255, 0xFF    # upper case digits
end

def test_underscore_separator
  assert_equal 1000000, 1_000_000
  assert_equal 0xff_ff,  0xffff
end

# ---------- Bignum threshold ----------

def test_large_bignum_fits_arithmetic
  big = 10 ** 30
  assert(big > 0, "Bignum should remain positive")
  assert_equal big, big * 1
  assert_equal 0,   big - big
end

def test_bignum_string_inspect
  big = 10 ** 30
  s = big.to_s
  assert_equal "1" + ("0" * 30), s
end

TESTS = [
  :test_decimal,
  :test_binary_literal,
  :test_octal_literal,
  :test_hex_literal,
  :test_underscore_separator,
  :test_large_bignum_fits_arithmetic,
  :test_bignum_string_inspect,
]
TESTS.each { |t| run_test(t) }
report "IntegerLiterals"
