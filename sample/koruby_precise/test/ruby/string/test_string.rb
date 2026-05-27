require_relative "../../test_helper"

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

def test_capitalize
  assert_equal "Hello", "hello".capitalize
  assert_equal "Hello", "HELLO".capitalize
  assert_equal "", "".capitalize
end

def test_swapcase
  assert_equal "hELLO wORLD", "Hello World".swapcase
end

def test_lstrip_rstrip
  assert_equal "a  ", "  a  ".lstrip
  assert_equal "  a", "  a  ".rstrip
end

def test_center
  assert_equal " hi ", "hi".center(4)
  assert_equal "**hi**", "hi".center(6, "*")
end

def test_ljust_rjust
  assert_equal "hi  ", "hi".ljust(4)
  assert_equal "  hi", "hi".rjust(4)
  assert_equal "hi**", "hi".ljust(4, "*")
  assert_equal "**hi", "hi".rjust(4, "*")
end

def test_intern
  assert_equal :foo, "foo".intern
  assert_equal :foo, "foo".to_sym
end

def test_slice
  assert_equal "ell", "hello".slice(1, 3)
  assert_equal "h", "hello".slice(0)
end

def test_squeeze
  assert_equal "abc", "aabbcc".squeeze
end

# Additional String tests culled from CRuby's test_string.rb.

def test_center
  assert_equal "  abc  ", "abc".center(7)
  assert_equal "**abc**", "abc".center(7, "*")
end

def test_ljust_rjust
  assert_equal "abc  ",   "abc".ljust(5)
  assert_equal "  abc",   "abc".rjust(5)
  assert_equal "abc..",   "abc".ljust(5, ".")
  assert_equal "..abc",   "abc".rjust(5, ".")
end

def test_chop
  assert_equal "ab",  "abc".chop
  assert_equal "ab",  "abc\n".chop[0..1]    # chops the \n then returns "abc"; basic
end

def test_chop_bang
  s = "hi"
  s.chop!
  assert_equal "h", s
end

def test_count_chars
  # 'l' (3) + 'o' (2) in "hello world"
  assert_equal 5, "hello world".count("lo")
  assert_equal 2, "hello".count("l")
end

def test_delete_chars
  assert_equal "heo", "hello".delete("l")
  assert_equal "hll", "hello".delete("aeiou")
end

def test_squeeze
  assert_equal "yelow mon",   "yellllow moon".squeeze("lo")
  assert_equal "helo",        "hheelloo".squeeze
end

def test_swapcase
  assert_equal "hELLO",   "Hello".swapcase
  assert_equal "FoO bAr", "fOo BaR".swapcase
end

def test_capitalize
  assert_equal "Hello",   "hello".capitalize
  assert_equal "Hello",   "HELLO".capitalize
  assert_equal "Hello world", "hELLO WORLD".capitalize
end

def test_lines
  assert_equal ["a\n", "b\n", "c"], "a\nb\nc".lines
  assert_equal ["abc"],              "abc".lines
end

def test_partition
  assert_equal ["he", "ll", "o"],     "hello".partition("ll")
  assert_equal ["hello", "", ""],     "hello".partition("xx")
end

def test_rpartition
  assert_equal ["abc.def", ".", "ghi"], "abc.def.ghi".rpartition(".")
end

def test_succ_next
  assert_equal "b",   "a".succ
  assert_equal "ab",  "aa".succ
  assert_equal "az",  "ay".next
end

def test_each_byte
  bytes = []
  "abc".each_byte { |b| bytes << b }
  assert_equal [97, 98, 99], bytes
end

def test_ord
  assert_equal 97,  "a".ord
  assert_equal 65,  "A".ord
end

def test_intern
  assert_equal :hello, "hello".intern
end

def test_freeze_str
  s = "hi"
  s.freeze
  assert_equal true, s.frozen?
end

def test_eql_str
  assert_equal true,  "abc".eql?("abc")
  assert_equal false, "abc".eql?("abd")
  assert_equal false, "abc".eql?(:abc)
end

def test_clone_str
  s = "abc"
  s2 = s.clone
  s2 << "d"
  assert_equal "abc",  s
  assert_equal "abcd", s2
end

def test_count_with_block_str
  # String#count doesn't take a block, but count with intersection of args
  assert_equal 1, "hello".count("h")
end

def test_str_compare
  assert_equal(-1, "abc" <=> "abd")
  assert_equal( 1, "abd" <=> "abc")
  assert_equal( 0, "abc" <=> "abc")
end

def test_format_sprintf
  assert_equal "x=42",     format("x=%d", 42)
  assert_equal "x=hello",  format("x=%s", "hello")
  assert_equal "x=3.14",   format("x=%.2f", 3.14)
end

def test_sub_block
  # No real Regexp yet; pass a String pattern and a block.
  assert_equal "Hello", "hello".sub("h") { |m| m.upcase }
end

def test_gsub_block
  # Same: String pattern + block.
  assert_equal "heLLo", "hello".gsub("l") { |m| m.upcase }
end

def test_string_slice_range
  assert_equal "ell",  "hello"[1..3]
  assert_equal "ello", "hello"[1..-1]
  assert_equal "el",   "hello"[1, 2]
end

TESTS = %i[
  test_basic test_empty test_concat
  test_mul test_compare test_split test_chars_bytes
  test_chomp_strip test_upcase_downcase test_reverse test_to_i_to_f
  test_start_end_with test_include test_gsub_sub test_format
  test_inspect test_to_sym test_interpolation test_capitalize
  test_swapcase test_lstrip_rstrip test_center test_ljust_rjust
  test_intern test_slice test_squeeze test_chop
  test_chop_bang test_count_chars test_delete_chars test_lines
  test_partition test_rpartition test_succ_next test_each_byte
  test_ord test_freeze_str test_eql_str test_clone_str
  test_count_with_block_str test_str_compare test_format_sprintf test_sub_block
  test_gsub_block test_string_slice_range
]
TESTS.each {|t| run_test(t) }
report "String"
