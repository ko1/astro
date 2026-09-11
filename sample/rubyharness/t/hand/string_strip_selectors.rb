# String#strip / lstrip / rstrip with character-selector args (Ruby 4.0):
# to_str coercion, encoding compatibility, invalid ranges, codepoint-wise matching.
p "  hello  ".lstrip(" ")
p "llo".lstrip("lo", "l")
p "hello".lstrip("ho", "h")
p "hell yeah".lstrip("")
p "ello".lstrip("aeiou", "^e")
p "hello".lstrip("^o")
p "hello".lstrip("e-h")
p "abcdefgh".lstrip("a-ce-fh")
p "四月".lstrip("四")
p "哥哥我倒".lstrip("哥")
p "哥哥我倒".rstrip("倒")
p "我倒哥哥".rstrip("哥")
p "月四月".strip("月")
p "a-b".lstrip("a\\-b")
p "^".lstrip("\\^")
p "\\".lstrip("\\\\")
p "xxhixx".strip("x")
s = "xxhixx"; p s.strip!("x"), s
s = "hi"; p s.lstrip!("x"), s
class Sel; def to_str; "h"; end; end
p "hello world".lstrip(Sel.new, "he")
begin; "hello".lstrip("h-e"); rescue ArgumentError => e; puts "ArgumentError: #{e.message}"; end
begin; "hello".lstrip("^h-e"); rescue ArgumentError => e; puts "ArgumentError: #{e.message}"; end
begin; "hello".lstrip(100); rescue TypeError => e; puts "TypeError: #{e.message}"; end
begin; "hello".rstrip([]); rescue TypeError => e; puts "TypeError: #{e.message}"; end
begin; "hello".lstrip("e".encode("UTF-16LE")); rescue Encoding::CompatibilityError => e; puts "CompatibilityError: #{e.message}"; end
begin; "hello".encode("UTF-16LE").lstrip("e"); rescue Encoding::CompatibilityError => e; puts "CompatibilityError: #{e.message}"; end
p "hello".encode("US-ASCII").lstrip("h").encoding
# whitespace strip must not eat other control bytes; NUL is stripped
p "\x01hello\x01".strip, "\0hi\0".strip, " \t\n\v\f\rhi\0 ".strip
s = "\x01hi\x01"; p s.strip!, s
