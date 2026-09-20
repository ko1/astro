# Character operations on the legacy multi-byte encodings koruby can step
# (Shift_JIS / Windows-31J / EUC-JP) instead of NotImplementedError.  #ord and
# #codepoints report the character's OWN bytes read big-endian, as CRuby does.
%w[EUC-JP Shift_JIS Windows-31J].each do |e|
  s = "あiうえお".encode(e)
  puts "#{e} len=#{s.length} bytes=#{s.bytesize}"
  p s[0].bytes
  p s[1]
  p s[2..3].bytes
  p s[-1].bytes
  p s[1, 2].bytes
  p s.reverse.bytes
  p s.ord
  p s.chars.map(&:bytesize)
  p s.each_char.to_a.size
  p s.codepoints
  p s.slice(1..2).bytes
  p s.length == s.chars.size
end

u = "あい".encode("UTF-16BE")
p [u.length, u.ord, u.codepoints]
v = "あい".encode("UTF-32LE")
p [v.length, v.ord, v.codepoints]

# a 4-byte UTF-8 character still reports its Unicode scalar
p "\u{1F600}a".codepoints
p "\u{1F600}a".ord
p "\u{1F600}a".length
