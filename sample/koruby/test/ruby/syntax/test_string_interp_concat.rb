require_relative "../../test_helper"

# String concatenation, %s formatting, and interpolation in non-trivial
# positions (string keys, array elements, etc.).

def test_plus_concat
  assert_equal "ab", "a" + "b"
end

def test_lshift_concat
  s = "a"
  s << "b"
  assert_equal "ab", s
end

def test_lshift_chains
  s = ""
  s << "a" << "b" << "c"
  assert_equal "abc", s
end

def test_string_mul
  assert_equal "ababab", "ab" * 3
  assert_equal "",       "x" * 0
end

# ---------- format / % ----------

def test_format_d
  assert_equal "x=42", "x=%d" % 42
  assert_equal "x=42", format("x=%d", 42)
end

def test_format_s
  assert_equal "hello world", "%s %s" % ["hello", "world"]
end

def test_format_padding
  assert_equal "  42", "%4d" % 42
  assert_equal "0042", "%04d" % 42
  assert_equal "42  ", "%-4d" % 42
end

def test_format_float_precision
  assert_equal "3.14", "%.2f" % 3.14159
  assert_equal "  3.14", "%6.2f" % 3.14159
end

# ---------- interpolation ----------

def test_interp_in_array
  x = 1
  result = ["a#{x}", "b#{x + 1}"]
  assert_equal ["a1", "b2"], result
end

def test_interp_in_hash_value
  k = "key"
  result = {a: "v#{k}"}
  assert_equal({a: "vkey"}, result)
end

def test_string_to_sym
  assert_equal :hello, "hello".to_sym
end

def test_sym_to_str
  assert_equal "hello", :hello.to_s
end

TESTS = [
  :test_plus_concat,
  :test_lshift_concat,
  :test_lshift_chains,
  :test_string_mul,
  :test_format_d,
  :test_format_s,
  :test_format_padding,
  :test_format_float_precision,
  :test_interp_in_array,
  :test_interp_in_hash_value,
  :test_string_to_sym,
  :test_sym_to_str,
]
TESTS.each { |t| run_test(t) }
report "StringInterpConcat"
