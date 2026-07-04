# String#[] / slice honour the encoding (byte-indexed for single-byte encodings,
# char-indexed for UTF-8); a slice keeps the source encoding. vs ruby.
s = "\xE3\x81\x82".b
p s[0].bytes
p s[1].bytes
p s[2].bytes
p s[-1].bytes
p s[0, 2].bytes
p s[1..2].bytes
p s[0].encoding.to_s
p "あ".b[0].bytes
p "café"[3]
p "café"[1, 2]
p "café"[1..2]
p "abcd".force_encoding("US-ASCII")[1, 2]
p "abcd".force_encoding("US-ASCII")[1].encoding.to_s
p "hello"[10]
