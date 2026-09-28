# a Regexp written in a single-byte encoding (Windows-1252 / ISO-8859-1 / BINARY) with high bytes compiles
# and matches byte-wise, also against an ASCII UTF-8 subject and through === (prawn line wrap)
src = "[^ \t\xAD-]+\xAD|x".force_encoding("Windows-1252")
r = Regexp.new(src); p r.encoding, r.source.encoding
s = "ab\xADcd".force_encoding("Windows-1252"); p s =~ r, s.scan(r)
l = Regexp.new("\xE9t\xE9".force_encoding("ISO-8859-1")); p "\xE9t\xE9".force_encoding("ISO-8859-1") =~ l, l.encoding
p (("été" =~ l) rescue $!.class)
p "abc" =~ r
b = Regexp.new("\xFF".b); p b.encoding, "\xFF".b =~ b
r = Regexp.new("\xE9".force_encoding("ISO-8859-1")); case "\xE9".force_encoding("ISO-8859-1"); when r then p :hit; end; p(r === "abc")
