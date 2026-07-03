# Per-string encoding tag (UTF-8 / US-ASCII / ASCII-8BIT): #encoding,
# #force_encoding, #b, chr/pack tagging. vs ruby.
p "abc".encoding.to_s
p "café".encoding.to_s
p "abc".b.encoding.to_s
p "abc".b.class.to_s
p "abc".force_encoding("US-ASCII").encoding.to_s
p "abc".force_encoding("BINARY").encoding.to_s
p "abc".force_encoding(Encoding::ASCII_8BIT).encoding.to_s
p 65.chr.encoding.to_s
p 200.chr.encoding.to_s
p [1,2,3].pack("C*").encoding.to_s
p "abc".dup.encoding.to_s
p "ab".force_encoding("US-ASCII").ascii_only?
s = "x"; r = s.force_encoding("BINARY"); p r.equal?(s)
p "abc".encoding.name
frozen = "y".freeze
begin; frozen.force_encoding("BINARY"); rescue FrozenError; p :frozen; end
p "abc".b.upcase.encoding.to_s
p "ABC".b.downcase.encoding.to_s
p "abc".b.reverse.encoding.to_s
p "abc".b.dup.encoding.to_s
p "a".force_encoding("US-ASCII").upcase.encoding.to_s
