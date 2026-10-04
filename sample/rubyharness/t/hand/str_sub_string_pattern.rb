# sub / gsub with a String pattern (escaped into a literal Regexp in a plain buffer): $~, multibyte, Shift_JIS
p "a.b.c".sub(".", "-"), "a.b.c".gsub(".", "-"), "x*y".gsub("*", "+"), "日本.語".sub(".", "!"), "^$".gsub("^", "v")
"ab(c)".sub("(c)", "Z"); p $~[0], $~.pre_match
long = "ab." * 200
p long.gsub("b.", "Q").size
p "aaa".sub("a") { |m| m.upcase }, "aaa".gsub("a") { $~.begin(0).to_s }
s = "xあy".encode("Shift_JIS"); p s.sub("あ".encode("Shift_JIS"), "z".encode("Shift_JIS")).encoding
