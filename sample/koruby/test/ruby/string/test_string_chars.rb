require_relative "../../test_helper"

# String methods: delete (multiple chars), tr_s, squeeze, count.

def test_delete_multiple_chars
  assert_equal "heo", "hello".delete("l")
  # delete("eo") removes 'e' and 'o' → "hll" (two l's remain).
  assert_equal "hll", "hello".delete("eo")
end

def test_squeeze_default
  assert_equal "abc", "aabbcc".squeeze
  assert_equal "a",   "aaaa".squeeze
end

def test_squeeze_specific
  assert_equal "ahllow",  "ahhhllow".squeeze("h")
end

def test_tr
  assert_equal "hippo", "hello".tr("el", "ip")
  assert_equal "HELLO", "hello".tr("a-z", "A-Z")
end

def test_tr_s
  # tr_s applies tr then squeezes runs of replaced chars
  assert_equal "xy",   "aaabbb".tr_s("ab", "xy")
  # "hello" → "hlxxo" (e→l, l→x), squeeze runs → "hlxo"
  assert_equal "hlxo", "hello".tr_s("el", "lx")
end

def test_count_basic
  assert_equal 2, "hello".count("l")
  assert_equal 3, "hello".count("el")
end

def test_count_empty
  assert_equal 0, "".count("a")
end

# ---------- chars / bytes ----------

def test_chars
  assert_equal ["a", "b", "c"], "abc".chars
  assert_equal [],              "".chars
end

def test_bytes
  assert_equal [97, 98, 99], "abc".bytes
end

def test_each_byte
  result = []
  "abc".each_byte { |b| result << b }
  assert_equal [97, 98, 99], result
end

TESTS = [
  :test_delete_multiple_chars,
  :test_squeeze_default,
  :test_squeeze_specific,
  :test_tr, :test_tr_s,
  :test_count_basic, :test_count_empty,
  :test_chars, :test_bytes, :test_each_byte,
]
TESTS.each { |t| run_test(t) }
report "StringChars"
