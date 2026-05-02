require_relative "../../test_helper"

# Misc String methods that were missing or limited.

def test_bytesize
  assert_equal 5, "hello".bytesize
  assert_equal 0, "".bytesize
end

def test_format_hash_keys
  result = "%{a}+%{b}" % {a: 1, b: 2}
  assert_equal "1+2", result
end

def test_format_hash_keys_repeated
  result = "%{x}-%{x}" % {x: "Y"}
  assert_equal "Y-Y", result
end

# ---------- chars / bytes / each_byte without block ----------

def test_each_char_no_block_returns_array
  # CRuby returns Enumerator; koruby returns Array as a stand-in.
  result = "abc".each_char.to_a
  assert_equal ["a", "b", "c"], result
end

# ---------- frozen-string operations ----------

def test_chars_on_frozen
  assert_equal ["x", "y"], "xy".freeze.chars
end

TESTS = [
  :test_bytesize,
  :test_format_hash_keys,
  :test_format_hash_keys_repeated,
  :test_each_char_no_block_returns_array,
  :test_chars_on_frozen,
]
TESTS.each { |t| run_test(t) }
report "StringExtras2"
