# 不正なバイト列を持つ String の case mapping は CRuby では
# ArgumentError("input string invalid")。ASCII-8BIT は例外で、
# ASCII の範囲だけ写して高位バイトはそのまま。
# (rack の MethodOverride が `method.to_s.upcase` の ArgumentError を拾う)
s = "\xBF".dup.force_encoding("UTF-8")
p s.valid_encoding?
%i[upcase downcase capitalize swapcase].each do |m|
  p(begin; s.public_send(m); rescue ArgumentError => e; [m, e.message]; end)
end
%i[upcase! downcase! capitalize! swapcase!].each do |m|
  t = s.dup
  p(begin; t.public_send(m); rescue ArgumentError => e; [m, e.message]; end)
end
# ASCII-8BIT はそのまま
b = "\xBF".b
p b.upcase.bytes
p b.downcase.bytes
p ("a\xBFz".b).upcase.bytes
# 正しい UTF-8 は今までどおり
p "straße".upcase
p "ÄÖÜ".downcase
p "abc".upcase
p "ABC".downcase
p "hello world".capitalize
p "AbC".swapcase
# US-ASCII に高位バイトがあるのも invalid
u = "\xBF".dup.force_encoding("US-ASCII")
p(begin; u.upcase; rescue ArgumentError => e; e.message; end)
# 反転 (op 4) は対象外
p s.reverse.bytes
