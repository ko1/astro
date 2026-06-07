# L0: string basics, concat, interpolation
p "abc" + "def"
p "ab" * 3
p "hello".length
p "hello".size
p "hello"[0]
p "hello"[1, 3]
p "hello"[-1]
p "hello"[1..3]
p "hello".upcase
p "HELLO".downcase
p "Hello".swapcase
p "hello".capitalize
p "  pad  ".strip
p "  pad  ".lstrip
p "  pad  ".rstrip
p "hello".reverse
p "hello".include?("ell")
p "hello".start_with?("he")
p "hello".end_with?("lo")
p "hello".index("l")
p "hello".rindex("l")
p "a,b,c".split(",")
p "a b c".split
p "hello".chars
p "hello".bytes
p "hello".each_char.to_a
p "hello world".sub("o", "0")
p "hello world".gsub("o", "0")
s = "hello"
p s.replace("bye")
p "hello".center(11, "-")
p "5".rjust(3, "0")
p "5".ljust(3, "0")
x = 7
p "x is #{x}"
p "sum is #{1 + 2}"
p "nested #{"inner #{x}"}"
p "abc".empty?
p "".empty?
p "hello" == "hello"
p "abc" <=> "abd"
p "abc".ord
p 97.chr
p "hello".to_sym
p "123".to_i
p "1.5".to_f
p "Hello\nWorld".lines
p "a-b-c".tr("-", "_")
