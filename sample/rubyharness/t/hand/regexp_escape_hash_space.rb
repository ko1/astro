# Regexp.escape / Regexp.union quote `#`, space, control characters as CRuby does.
p Regexp.escape("a#b {x} -.?*+^$|()[]\\/ \t\n\r\f\v\0z"), Regexp.escape(:"a b")
p Regexp.union("#", "a b", "x\ty", /re/i), Regexp.union("a#b").match?("a#b"), /#{Regexp.escape("#x y")}/.match?("#x y")
p Regexp.union("é.", "#").source, Regexp.escape("é#").encoding, Regexp.escape("ab").encoding
