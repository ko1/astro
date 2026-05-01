require_relative "test_helper"

def test_basic
  s = "hello"
  assert_equal 5, s.size
  assert_equal 5, s.length
  assert_equal "h", s[0]
  assert_equal "o", s[-1]
  assert_equal "el", s[1, 2]
end

def test_empty
  assert_equal true, "".empty?
  assert_equal false, "a".empty?
end

def test_concat
  a = "hello"
  b = " world"
  assert_equal "hello world", a + b
  s = "x"
  s << "y"; s << "z"
  assert_equal "xyz", s
end

def test_mul
  assert_equal "abcabcabc", "abc" * 3
  assert_equal "", "abc" * 0
end

def test_compare
  assert_equal true, "abc" == "abc"
  assert_equal false, "abc" == "abd"
  assert_equal -1, ("abc" <=> "abd")
  assert_equal 0, ("abc" <=> "abc")
  assert_equal 1, ("abd" <=> "abc")
end

def test_split
  assert_equal ["a", "b", "c"], "a,b,c".split(",")
  assert_equal ["a", "b", "c"], "a b c".split(" ")
  assert_equal ["a", "b", "c"], "abc".split("")
  assert_equal ["abc"], "abc".split(",")
end

def test_chars_bytes
  assert_equal ["a", "b", "c"], "abc".chars
  assert_equal [97, 98, 99], "abc".bytes
end

def test_chomp_strip
  assert_equal "hello", "hello\n".chomp
  assert_equal "hello", "hello".chomp
  assert_equal "hello", "  hello  ".strip
end

def test_upcase_downcase
  assert_equal "HELLO", "hello".upcase
  assert_equal "hello", "HELLO".downcase
end

def test_reverse
  assert_equal "cba", "abc".reverse
end

def test_to_i_to_f
  assert_equal 42, "42".to_i
  assert_equal 0, "abc".to_i
  assert_equal 3.14, "3.14".to_f
end

def test_start_end_with
  assert_equal true, "hello".start_with?("he")
  assert_equal false, "hello".start_with?("ll")
  assert_equal true, "hello".end_with?("lo")
  assert_equal false, "hello".end_with?("he")
  # multiple args
  assert_equal true, "hello".start_with?("xx", "he", "yy")
end

def test_include
  assert_equal true, "hello".include?("ell")
  assert_equal false, "hello".include?("xyz")
end

def test_gsub_sub
  assert_equal "hxxlo", "hello".gsub("el", "xx")
  assert_equal "hexxo", "hello".gsub("l", "x").sub("xx", "xx") # idempotent stub
  assert_equal "hexxo", "hello".sub("ll", "xx")
  assert_equal "hexxo", "hello".gsub("ll", "xx")
end

def test_format
  assert_equal "hi 42", "hi %d" % 42
  assert_equal "abc 3.14", "abc %.2f" % 3.14
  assert_equal "ff", "%x" % 255
  assert_equal "00FF", "%04X" % 255
end

def test_inspect
  assert_equal '"hello"', "hello".inspect
end

def test_to_sym
  assert_equal :foo, "foo".to_sym
end

def test_interpolation
  x = 42
  s = "value=#{x}"
  assert_equal "value=42", s
end

TESTS = %i[
  test_basic test_empty test_concat test_mul test_compare
  test_split test_chars_bytes test_chomp_strip test_upcase_downcase
  test_reverse test_to_i_to_f test_start_end_with test_include
  test_gsub_sub test_format test_inspect test_to_sym test_interpolation
]
TESTS.each {|t| run_test(t) }
report("String")
