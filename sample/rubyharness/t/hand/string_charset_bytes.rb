# String#count / delete / tr / squeeze / strip on a single-byte receiver (binary,
# Latin-1) take the sets as bytes: "\xf0-\xff" is a byte range (rubocop counts
# 4-byte UTF-8 leads that way for LSP UTF-16 columns).
s = "'☃🍣🍺' x"
p s.b.count("\xf0-\xff".b), s.b.count("\x80-\xbf".b), s.b.count("a-z"), s.b.count("^\x00-\x7f".b)
p "abc\xff".b.count("\xff".b), "hello".count("l-o"), "héllo".count("é"), "héllo".count("^a-z")
p s.b.delete("\xf0-\xff".b).bytesize, s.b.tr("\xf0-\xff".b, "X").count("X"), s.b.squeeze("\x80-\xff".b).bytesize
s = "caf\xE9 \xE9t\xE9".force_encoding("ISO-8859-1")
p s.count("\xE9".force_encoding("ISO-8859-1")), s.delete("\xE9".force_encoding("ISO-8859-1")), s.tr("\xE9".force_encoding("ISO-8859-1"), "e")
b = "\x00\x01\xFF\xFE ab".b
p b.tr("\x00-\x1F".b, "."), b.tr("\xF0-\xFF".b, "*"), b.tr_s("\xF0-\xFF".b, "*"), b.squeeze, b.strip, "\xFFab\xFF".b.delete("\xFF".b)
p "héllo wörld".count("ö-ü"), "héllo".tr("é", "e"), "  x　".strip, "aaa\xFF\xFF".b.squeeze("\xFF".b)
p "\xE2\x98\x83".b.count("\xE2\x98\x83".b), "\xE2\x98\x83".b.tr("\x80-\xFF".b, "x")
