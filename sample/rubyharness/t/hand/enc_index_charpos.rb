# String#index / #rindex / =~ / MatchData#begin report a CHARACTER index, and
# their optional start position is a character index too.  koruby converted
# between bytes and characters with the UTF-8 rule whatever the subject's
# encoding was, so on EUC-JP / Shift_JIS / UTF-16 / UTF-32 subjects every one of
# them came back as a byte offset (or a UTF-8 miscount of one).
encs = %w[UTF-8 EUC-JP Shift_JIS UTF-16BE UTF-16LE UTF-32LE US-ASCII ASCII-8BIT]
encs.each do |e|
  s = begin; "あいうえお".encode(e); rescue; "abcde".encode(e); end
  n = s[2]
  p [e, s.index(n), s.rindex(n), s.index(n, 1), s.index(n, -3), s.rindex(n, 3)]
  p [e, s.index(s[0]), s.index(s[4]), s.index(s[0], 1), s.rindex(s[4], 2)]
  p [e, s.partition(n).map(&:bytesize), s.split(n).size, s.include?(n)]
end

# Regexp positions follow the same rule (single-byte subjects only here: the
# matcher itself is byte-oriented, so a multibyte pattern is a separate matter).
%w[UTF-8 EUC-JP US-ASCII].each do |e|
  s = begin; "aあbあc".encode(e); rescue; next; end
  re = Regexp.new("b".encode(e))
  m = re.match(s)
  p [e, s =~ re, m && [m.begin(0), m.end(0), m.byteoffset(0)], s.index(re), s.rindex(re)]
  p [e, s.index(re, 1), s.match(re, 1)&.begin(0), re.match(s, 1)&.begin(0), re.match?(s, 1)]
end

# byteindex/byterindex stay in bytes
s = "あいう"
p [s.byteindex("い"), s.byterindex("い"), s.index("い"), s.rindex("い")]

# ljust / rjust / center and String#insert count characters too
%w[UTF-8 EUC-JP Shift_JIS UTF-16BE UTF-32LE ASCII-8BIT].each do |e|
  s = (begin; "あいうえお".encode(e); rescue; "abcde".encode(e); end)
  p [e, s.ljust(9, s[0]).size, s.rjust(9, s[0]).size, s.center(9, s[0]).size, s.center(9, s[0]).bytesize]
  [-6, -1, 0, 2, 5, 6].each do |i|
    d = s.dup
    p [e, i, (begin; d.insert(i, s[0]); d.bytes; rescue => x; x.class; end)]
  end
end
p ["あいう".to_sym.length, "あいう".encode("EUC-JP").to_sym.length, :abc.length]

# String#* keeps the encoding; a String pattern becomes a Regexp in the
# receiver's encoding; chop / split("") / upcase step characters; strip does not
# eat the NUL bytes inside a UTF-16/32 code unit.
%w[UTF-8 EUC-JP Shift_JIS Windows-31J UTF-16BE UTF-16LE UTF-32LE UTF-32BE US-ASCII ASCII-8BIT ISO-8859-1].each do |e|
  s = (begin; "あbいcう".encode(e); rescue; "XbYcZ".encode(e); end)
  p [e, (s * 2).encoding.name, (s * 2).bytesize, (s * 0).encoding.name]
  p [e, s.chop.bytes, s.split("").map(&:bytes), s.split(s[2]).map(&:bytes)]
  p [e, s.upcase.bytes, s.downcase.bytes, s.swapcase.bytes, s.capitalize.bytes]
  p [e, (begin; s.sub(s[2], s[0]).bytes; rescue => x; x.class; end)]
  p [e, (s[0] + s + s[0]).strip.bytes, s.rstrip.bytes, s.lstrip.bytes]
  p [e, s.casecmp?(s.dup), s.casecmp(s.dup)]
end
# Shift_JIS trail bytes that look like ASCII letters must survive case mapping
sj = "\x83\x41\x83\x61\x82\x60".dup.force_encoding("Shift_JIS")
p [sj.valid_encoding?, sj.upcase.bytes, sj.downcase.bytes, sj.swapcase.bytes]
