# 単バイト encoding の文字 index はバイト index。UTF-8 と混ぜて確認する。
b = "aé b\xFFc".dup.force_encoding("BINARY")
u = "aé b c"
[["BINARY", b], ["UTF-8", u]].each do |name, s|
  p [name, s.length, s.index("b"), s.rindex("c"), s.index("b", 1), s.rindex("c", -1)]
  p [s.index(/b/), s.rindex(/c/), s =~ /b/]
  p [s.index("zz"), s.rindex("zz")]
end
a = "aaa".dup.force_encoding("US-ASCII")
p [a.index("a", 1), a.rindex("a", 1)]
big = ("\xE3\x81\x82" * 5).dup.force_encoding("BINARY") + "X"
p [big.index("X"), big.rindex("X"), big.length]
big2 = ("あ" * 5) + "X"
p [big2.index("X"), big2.rindex("X"), big2.length]
