s = "\x01\x00\x02\x00\x03\x00\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d".b
p s.unpack("v3C4V2v"), s[0, 14].unpack("v3C4V2v")
p "ab".unpack("Nn"), "abc".unpack("VCC"), "abcdef".unpack("V2v"), "abcdef".unpack("q v"), "ab".unpack("a3 v")
p "abcde".unpack("V*"), "abcde".unpack("v*")
