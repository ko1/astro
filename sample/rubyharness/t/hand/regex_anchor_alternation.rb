# A leading `\A` only anchors ITS branch: a top-level alternation still has to
# be tried at every position ("abc".gsub(/\Aa|c\z/) must replace both ends).
p "'abc }'".gsub(/\A'|'\z/, '"')
p "'abc }'".scan(/\A'|'\z/)
p "abc".gsub(/\Aa|c\z/, "-")
p "abc".scan(/\Aa|c\z/)
p "xa'".gsub(/\A'|'\z/, '"')
p "a\nc".gsub(/\Aa|c\z/, "-")
p "aXc".sub(/c\z/, "-")
p "abc".index(/\Aa|c\z/)
p "abc".rindex(/\Aa|c\z/)

# a genuinely anchored pattern still only matches at the start
p ["abc".gsub(/\Aa/, "-"), "aba".gsub(/\Aa/, "-"), "bab".scan(/\Ab/)]
p ["abc" =~ /\A(a|b)/, "bac" =~ /\A(a|b)/, "cab" =~ /\A(a|b)/]
p "aaa".gsub(/\Aa|a\z/, "-")
p "abcabc".scan(/\Aabc|abc\z/)
