class S; def to_str; "ABC"; end; end
p "abc".casecmp(S.new)
p "abc".casecmp?(S.new)
p "abc".casecmp("ABC")
p "abc".casecmp(123)
p "abc".casecmp?(456)
p "abD".casecmp("abc")
