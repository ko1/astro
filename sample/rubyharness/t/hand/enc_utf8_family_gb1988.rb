# UTF8-KDDI / DoCoMo / SoftBank / MAC are UTF-8 underneath; GB1988 is single-byte (not a GB multi-byte one)
e = "UTF8-KDDI"
s = "\u{E468}a".force_encoding(e)
p s.encoding, s.ord, s.valid_encoding?, s.length, s.chars.size, s.codepoints, s.reverse.bytes
g = "ab\xE0".force_encoding("GB1988"); p g.length, g.ord, g.chars.size, g.reverse.bytes
m = "caf\u00e9".force_encoding("UTF8-MAC"); p m.length, m.codepoints
