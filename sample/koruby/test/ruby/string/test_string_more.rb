require_relative "../../test_helper"

# String methods that were missing or broken in koruby:
# hex, oct, prepend, insert, delete_prefix, delete_suffix, each_line
# (was returning empty without iterating).

# ---------- hex / oct ----------

def test_hex
  assert_equal 255, "0xff".hex
  assert_equal 255, "ff".hex
  assert_equal -16, "-0x10".hex
  assert_equal 0,   "".hex
  assert_equal 0,   "garbage".hex
end

def test_oct
  assert_equal 17,  "021".oct           # default base if no prefix is octal
  assert_equal 15,  "0o17".oct
  assert_equal 255, "0xff".oct          # 0x prefix → hex
  assert_equal 5,   "0b101".oct         # 0b prefix → binary
  assert_equal 0,   "".oct
end

# ---------- prepend / insert ----------

def test_prepend
  s = "World"
  s.prepend("Hello, ")
  assert_equal "Hello, World", s
end

def test_insert_positive_index
  s = "abcd"
  s.insert(1, "X")
  assert_equal "aXbcd", s
end

def test_insert_negative_index
  s = "abcd"
  s.insert(-2, "X")             # before the last char
  assert_equal "abcXd", s
end

# ---------- delete_prefix / delete_suffix ----------

def test_delete_prefix
  assert_equal "llo",  "hello".delete_prefix("he")
  assert_equal "hello", "hello".delete_prefix("xx")  # no match → original
end

def test_delete_suffix
  assert_equal "hel",   "hello".delete_suffix("lo")
  assert_equal "hello", "hello".delete_suffix("xx")
end

# ---------- each_line ----------

def test_each_line
  collected = []
  "a\nb\nc".each_line { |l| collected << l }
  assert_equal ["a\n", "b\n", "c"], collected
end

def test_each_line_no_trailing_newline
  collected = []
  "a\nb".each_line { |l| collected << l }
  assert_equal ["a\n", "b"], collected
end

TESTS = [
  :test_hex, :test_oct,
  :test_prepend,
  :test_insert_positive_index, :test_insert_negative_index,
  :test_delete_prefix, :test_delete_suffix,
  :test_each_line, :test_each_line_no_trailing_newline,
]
TESTS.each { |t| run_test(t) }
report "StringMore"
