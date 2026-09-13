# encoding: binary
# Ruby 4.1 String#bitwise_* (and the bang forms).  Differential vs CRuby 4.1.0dev.
Warning[:experimental] = false rescue nil   # IO::Buffer is still experimental in CRuby

def t(label)
  print label.ljust(46), " => "
  p(yield)
rescue Exception => e
  puts "#{e.class}: #{e.message}"
end

puts "-- bitwise_and / or / xor"
t("and") { "\xF0".bitwise_and("\xCC").bytes }
t("or") { "\xF0".bitwise_or("\x0C").bytes }
t("xor") { "\xF0".bitwise_xor("\xCC").bytes }
t("and is a fresh String") { s = "\xF0"; r = s.bitwise_and("\xCC"); [r.equal?(s), s.bytes, r.frozen?] }
t("and of 17 bytes") { (("\xF0" * 17).bitwise_and("\xCC" * 17)).bytes.uniq }
t("empty operands") { "".bitwise_and("").bytes }
t("result is BINARY (and)") { (+"\xF0").force_encoding("UTF-8").bitwise_and("\xCC").encoding }
t("result is BINARY (or)") { (+"\xF0").force_encoding("UTF-8").bitwise_or("\x0C").encoding }
t("result is BINARY (xor)") { (+"\xF0").force_encoding("UTF-8").bitwise_xor("\xCC").encoding }
t("frozen receiver is fine") { "\xF0".freeze.bitwise_and("\xCC").bytes }
t("#to_str operand") do
  o = Object.new
  def o.to_str = "\xCC".dup
  "\xF0".bitwise_and(o).bytes
end
t("shorter operand") { "\xF0".bitwise_and("") }
t("longer operand") { "\xF0".bitwise_and("\x00\x00") }
t("nil operand") { "\xF0".bitwise_and(nil) }
t("Integer operand") { "\xF0".bitwise_and(1) }
t("Symbol operand") { "\xF0".bitwise_and(:a) }
t("arity") { "".method(:bitwise_and).arity }
t("no operand") { "\xF0".bitwise_and }
t("two operands") { "\xF0".bitwise_and("\x00", "\x00") }

puts "-- bitwise_not"
t("not") { "\x00\xAA".bitwise_not.bytes }
t("not is a fresh String") { s = "\x00\xAA"; r = s.bitwise_not; [r.equal?(s), s.bytes] }
t("not of empty") { "".bitwise_not.bytes }
t("not result is BINARY") { (+"\x00").force_encoding("US-ASCII").bitwise_not.encoding }
t("not arity") { "".method(:bitwise_not).arity }
t("not with an argument") { "\x00".bitwise_not(1) }

puts "-- the bang forms"
t("and!") { s = +"\xF0"; [s.bitwise_and!("\xCC").equal?(s), s.bytes] }
t("or!") { s = +"\xF0"; [s.bitwise_or!("\x0C").equal?(s), s.bytes] }
t("xor!") { s = +"\xF0"; [s.bitwise_xor!("\xCC").equal?(s), s.bytes] }
t("not!") { s = +"\x00\xAA"; [s.bitwise_not!.equal?(s), s.bytes] }
t("and! keeps the encoding") { s = (+"\xF0").force_encoding("UTF-8"); s.bitwise_and!("\xCC"); s.encoding }
t("not! keeps the encoding") { s = (+"\x00").force_encoding("UTF-8"); s.bitwise_not!; s.encoding }
t("and! on frozen") { "\x00".freeze.bitwise_and!("\x00") }
t("or! on frozen") { "\x00".freeze.bitwise_or!("\x00") }
t("xor! on frozen") { "\x00".freeze.bitwise_xor!("\x00") }
t("not! on frozen") { "\x00".freeze.bitwise_not! }
t("and! length mismatch") { (+"\xF0").bitwise_and!("\x00\x00") }
t("frozen AND length mismatch") { "\xF0".freeze.bitwise_and!("\x00\x00") }
t("not! on empty") { (+"").bitwise_not!.bytes }
t("and! with #to_str") do
  o = Object.new
  def o.to_str = "\xCC".dup
  s = +"\xF0"
  s.bitwise_and!(o)
  s.bytes
end

puts "-- an exhaustive byte table"
t("and table") { (0..255).map { |b| "#{b.chr}".bitwise_and("\x5A").bytes[0] }.sum }
t("or table") { (0..255).map { |b| "#{b.chr}".bitwise_or("\x5A").bytes[0] }.sum }
t("xor table") { (0..255).map { |b| "#{b.chr}".bitwise_xor("\x5A").bytes[0] }.sum }
t("not table") { (0..255).map { |b| "#{b.chr}".bitwise_not.bytes[0] }.sum }
t("bit_count table") { (0..255).map { |b| "#{b.chr}".bit_count }.sum }

puts "-- Integer#bit_count"
t("small") { [0, 1, 2, 3, 7, 0b10101, 0xff, 0x100].map(&:bit_count) }
t("fixnum max") { m = 2**62 - 1; m.bit_count == m.to_s(2).count("1") }
t("2**1000") { (2**1000).bit_count }
t("2**1000 - 1") { (2**1000 - 1).bit_count }
t("2**1000 + 2**500") { (2**1000 + 2**500).bit_count }
t("2**64 - 1") { (2**64 - 1).bit_count }
t("2**10000 - 1") { (2**10000 - 1).bit_count }
t("class of a bignum count") { (2**1000).bit_count.class }
t("-1") { -1.bit_count }
t("-19") { -19.bit_count }
t("-(2**1000)") { (-2**1000).bit_count }
t("-(2**1000) - 1") { (-2**1000 - 1).bit_count }
t("arity") { 1.method(:bit_count).arity }
t("with an argument") { 1.bit_count(2) }

puts "-- IO::Buffer#bit_count"
t("whole buffer") { IO::Buffer.for(+"\xFF\x00\x0F") { |b| b.bit_count } }
t("all zero") { IO::Buffer.for(+"\x00\x00\x00") { |b| b.bit_count } }
t("9 x 0xFF") { IO::Buffer.for(+"\xFF" * 9) { |b| b.bit_count } }
t("empty") { IO::Buffer.new(0).bit_count }
t("8-byte aligned") { IO::Buffer.for(+"\xAA" * 8) { |b| b.bit_count } }
t("offset only") { IO::Buffer.for(+"\xFF\x00\x0F") { |b| [b.bit_count(0), b.bit_count(1), b.bit_count(2), b.bit_count(3)] } }
t("offset + length") { IO::Buffer.for(+"\xFF\x00\x0F") { |b| [b.bit_count(0, 1), b.bit_count(1, 1), b.bit_count(2, 1), b.bit_count(1, 2)] } }
t("returns an Integer") { IO::Buffer.for(+"\xFF") { |b| b.bit_count.is_a?(Integer) } }
t("past the end") { IO::Buffer.for(+"\xFF") { |b| b.bit_count(0, 2) } }
t("offset past the end") { IO::Buffer.for(+"\xFF") { |b| b.bit_count(1, 1) } }
t("negative offset") { IO::Buffer.for(+"\xFF") { |b| b.bit_count(-1) } }
t("negative length") { IO::Buffer.for(+"\xFF") { |b| b.bit_count(0, -1) } }
t("three arguments") { IO::Buffer.for(+"\xFF") { |b| b.bit_count(0, 1, 2) } }
