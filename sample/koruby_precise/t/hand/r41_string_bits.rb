# encoding: binary
# Ruby 4.1 String bit accessors.  Differential vs CRuby 4.1.0dev.
def t(label)
  print label.ljust(46), " => "
  p(yield)
rescue Exception => e
  puts "#{e.class}: #{e.message}"
end

puts "-- bit_count"
t("''") { "".bit_count }
t("\\x00") { "\x00".bit_count }
t("\\xFF") { "\xFF".bit_count }
t("\\xAA\\xF0") { "\xAA\xF0".bit_count }
t("9 bytes of \\xFF") { ("\xFF" * 9).bit_count }
t("17 bytes of \\xAA") { ("\xAA" * 17).bit_count }
t("multibyte source") { "あい".dup.force_encoding("UTF-8").bit_count }
t("arity") { "".method(:bit_count).arity }
t("bit_count(0)") { "\x00".bit_count(0) }
t("bit_count(lsb_first: false)") { "\x00".bit_count(lsb_first: false) }

puts "-- bit_get / bit_set?"
t("\\xAA lsb 0..7") { (0..7).map { |i| "\xAA".bit_get(i) } }
t("\\xAA msb 0..7") { (0..7).map { |i| "\xAA".bit_get(i, lsb_first: false) } }
t("\\xAA lsb_first: true 0") { "\xAA".bit_get(0, lsb_first: true) }
t("2-byte msb 8") { "\x00\x80".bit_get(8, lsb_first: false) }
t("2-byte lsb 8..15") { (8..15).map { |i| "\x00\x80".bit_get(i) } }
t("past the end") { "\x00".bit_get(8) }
t("empty string") { "".bit_get(0) }
t("far past the end") { "\x00".bit_get(1 << 40) }
t("negative") { "\x00".bit_get(-1) }
t("huge bignum offset") { "\x00".bit_get(1 << 70) }
t("negative bignum offset") { "\x00".bit_get(-(1 << 70)) }
t("Float offset") { "\xAA".bit_get(1.0) }
t("String offset") { "\xAA".bit_get("1") }
t("to_int offset") { o = Object.new; def o.to_int = 1; "\xAA".bit_get(o) }
t("no args") { "\xAA".bit_get }
t("lsb_first: nil") { "\xAA".bit_get(0, lsb_first: nil) }
t("lsb_first: 1") { "\xAA".bit_get(0, lsb_first: 1) }
t("unknown keyword") { "\xAA".bit_get(0, bogus: true) }
t("frozen receiver reads fine") { "\xAA".freeze.bit_get(1) }
t("bit_set? \\xAA lsb 0..7") { (0..7).map { |i| "\xAA".bit_set?(i) } }
t("bit_set? \\xAA msb 0..7") { (0..7).map { |i| "\xAA".bit_set?(i, lsb_first: false) } }
t("bit_set? past the end") { "\x00".bit_set?(8) }
t("bit_set? negative") { "\x00".bit_set?(-1) }
t("bit_set? lsb_first: nil") { "\x00".bit_set?(0, lsb_first: nil) }

puts "-- bit_set / bit_clear / bit_flip"
t("set lsb 1") { s = +"\x00"; [s.bit_set(1).equal?(s), s.bytes] }
t("set msb 1") { s = +"\x00"; s.bit_set(1, lsb_first: false); s.bytes }
t("set msb 8 of 2") { s = +"\x00\x00"; s.bit_set(8, lsb_first: false); s.bytes }
t("clear lsb 1") { s = +"\xFF"; [s.bit_clear(1).equal?(s), s.bytes] }
t("clear msb 1") { s = +"\xFF"; s.bit_clear(1, lsb_first: false); s.bytes }
t("clear msb 8 of 2") { s = +"\xFF\xFF"; s.bit_clear(8, lsb_first: false); s.bytes }
t("flip twice") { s = +"\x00"; s.bit_flip(1); a = s.bytes; s.bit_flip(1); [a, s.bytes] }
t("flip msb 1") { s = +"\x00"; s.bit_flip(1, lsb_first: false); s.bytes }
t("flip msb 8 of 2") { s = +"\x00\x00"; s.bit_flip(8, lsb_first: false); s.bytes }
t("set keeps the encoding") { s = (+"\x00").force_encoding("UTF-8"); s.bit_set(0); s.encoding }
t("set out of range") { (+"\x00").bit_set(8) }
t("set negative") { (+"\x00").bit_set(-1) }
t("set on an empty string") { (+"").bit_set(0) }
t("set huge bignum") { (+"\x00").bit_set(1 << 70) }
t("set frozen") { "\x00".freeze.bit_set(0) }
t("clear frozen") { "\x00".freeze.bit_clear(0) }
t("flip frozen") { "\x00".freeze.bit_flip(0) }
t("frozen AND out of range") { "\x00".freeze.bit_set(8) }
t("frozen AND negative") { "\x00".freeze.bit_set(-1) }
t("set lsb_first: nil") { (+"\x00").bit_set(0, lsb_first: nil) }
t("set two positionals") { (+"\x00").bit_set(0, false) }

puts "-- every bit of a 2-byte string, both directions"
t("lsb round trip") do
  s = +"\x00\x00"
  16.times { |i| s.bit_set(i) }
  a = s.bytes
  16.times { |i| s.bit_clear(i) }
  [a, s.bytes]
end
t("msb round trip") do
  s = +"\x00\x00"
  16.times { |i| s.bit_set(i, lsb_first: false) }
  a = s.bytes
  16.times { |i| s.bit_clear(i, lsb_first: false) }
  [a, s.bytes]
end
t("lsb/msb disagree per byte") do
  (0..15).map { |i| s = +"\x00\x00"; s.bit_set(i, lsb_first: false); s.bytes }
end
