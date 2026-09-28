# regexps over Shift_JIS / EUC-JP subjects step whole characters (scan / partition / split
# no longer skip a lead byte in 0x80..0xBF); "sjis" is Windows-31J and a same-encoding
# encode(invalid: :replace) only scrubs
s = "a仝b仝c".encode("sjis")
p s.scan(/./).map(&:bytes), s.scan(/\X/).size, s.scan(//).size
p s.rpartition(/./).map(&:bytes), s.partition(/b/).map(&:bytes)
p s.split(//).map(&:bytes), s.gsub(//, "-").bytes
p s =~ /c/, (/c/ === s), s.index(/c/), s.byteindex(/c/)
e = "x仝y".encode("euc-jp"); p e.scan(/./).size, e =~ /y/
bad = "a\x81".force_encoding("sjis"); p(begin; bad =~ /x/; rescue => e; e; end)
u = "\x82\xa0".force_encoding("sjis"); p u.scan(/./).map(&:bytes)
sj = "仝".encode("sjis"); p "a仝b".encode("sjis") =~ Regexp.new(sj)
p "ab".encode("sjis").scan(/\w/), "仝".encode("sjis").match?(/\w/)
p "a\xFFb".encode("UTF-8", invalid: :replace), "a\xFFb".encode("utf-8", "UTF-8", invalid: :replace, replace: "*")
p "a\xE3\x81b".encode("UTF-8", invalid: :replace)
s = 60777.chr("sjis") + "\xFF".b.force_encoding("sjis")
p s.encode("sjis", invalid: :replace, undef: :replace).bytes, s.encode("CP932", invalid: :replace).bytes
p s.encode("sjis").bytes
p "\xFF\xFE".force_encoding("UTF-16LE").encode("UTF-16LE", invalid: :replace).bytes
p "a\xFF".encode("US-ASCII", "ASCII", invalid: :replace)
