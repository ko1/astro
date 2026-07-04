# String#chomp("\n") strips the universal line ending (\r\n, \n, or \r); other
# separators strip a literal trailing match; the arg coerces via #to_str. vs ruby.
p "abc\r\r".chomp("\n")
p "abc\r\n".chomp("\n")
p "abc\n".chomp("\n")
p "abc\r".chomp("\n")
p "abc\r\n\r".chomp("\n")
p "hello".chomp("lo")
p "abcabc".chomp("abc")
class ToStr; def to_str; "c"; end; end
p "abc".chomp(ToStr.new)
p "abc".chomp("")
