# `Regexp.new(src, true)` は文書化された boolean 形なので警告しない
# (警告するのは true/false/nil/Integer/String 以外のとき)。optparse が使う。
p Regexp.new("a", true)
p Regexp.new("a", Regexp::IGNORECASE)
p Regexp.new("a", nil)
p Regexp.new("a", false)
p Regexp.new("a", "x")
p Regexp.new("a", true).match?("A")
p Regexp.new("a", false).match?("A")
