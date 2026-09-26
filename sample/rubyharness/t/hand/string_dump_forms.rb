# String#dump escapes (plain bytes are copied directly), the Ruby 4.1
# `.dup.force_encoding("...")` suffix for non-ASCII-compatible encodings, and
# #undump reading both suffix forms back.
p "ab\"c\\d#{1}\#{x}#$y#@z\n\t\r\a\b\v\f\e\x01\x7f é 😀 end".dump
p "x".b.dump, "\xff\xfe".dump, ("q" * 5000 + "\n" * 3000).dump.size
s = "é".encode("UTF-16LE")
d = s.dump
p d, d.undump == s, d.undump.encoding
p "\"a\".dup.force_encoding(\"UTF-8\")".undump, "\"a\".force_encoding(\"UTF-8\")".undump
begin; "\"a\".dup".undump; rescue => e; p e.class; end
p (0..255).map(&:chr).join.b.dump.undump.bytes == (0..255).to_a
