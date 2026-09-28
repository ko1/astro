# String#inspect escapes a UTF-8-encoded surrogate / overlong / > U+10FFFF sequence as bytes
a = [0xDC00].pack('U'); p a, a.bytes, a.valid_encoding?, a.encoding
s = +"x\\uDC00y"
s.gsub!(/\\u([\da-f]{4})/i) { [$1.to_i(16)].pack('U') }
p s, s.valid_encoding?
p ([0xDC00].pack('U') + [0xDF2C].pack('U')).inspect
p [0x110000].pack('U') rescue p $!
p "\xC0\x80", "\xE0\x80\x80", "\xF0\x80\x80\x80", "é", "\u{10FFFF}", "\xED\x9F\xBF", "\xEE\x80\x80", "a\xF4\x8F\xBF\xBFb", "\xE3\x81"
p ["\xED\xA0\x80"], {k: "\xF5\x80\x80\x80"}
