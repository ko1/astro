# Exercises every path this change touches, under GC stress: non-UTF-8 #inspect,
# Symbol#inspect/interning, Regexp#source/#encoding, valid_encoding?, set_encoding.
ok = 0
300.times do |i|
  s = "\u{3042}#{i}".encode("EUC-JP")
  raise "euc #{s.inspect}" unless s.inspect.start_with?('"\\x{A4A2}')
  u = "hello#{i} привет".encode("utf-16le")
  raise "u16 #{u.inspect}" unless u.inspect == "\"hello#{i} \\u043F\\u0440\\u0438\\u0432\\u0435\\u0442\""
  raise "bin" unless "foo\xA4#{i}".b.to_sym.inspect.start_with?(':"foo\\xA4')
  raise "u16sym" unless "foo#{i}".encode("UTF-16LE").to_sym.inspect == %{:"foo#{i}"}
  raise "dummy" unless "abcd".dup.force_encoding("ISO-2022-JP").to_sym.inspect == ':"\x61\x62\x63\x64"'
  re = Regexp.new("a#{i}")
  raise "src" unless re.source.encoding == re.encoding
  raise "srcv" unless re.source == "a#{i}"
  raise "alloc" unless Regexp.allocate.encoding == Encoding::BINARY
  raise "fixed" unless /needle \u{8768}/.fixed_encoding?
  raise "ve1" if "\xE6\x9D\x94".dup.force_encoding("EUC-JP").valid_encoding?
  raise "ve2" unless "\xE6\x9D\x94".dup.force_encoding("ISO-8859-1").valid_encoding?
  raise "ve3" if "\x00\xD8".dup.force_encoding("UTF-16LE").valid_encoding?
  ec = Encoding::Converter.new("utf-8", "us-ascii")
  ec.replacement = "!#{i % 10}"
  raise "repl" unless ec.replacement == "!#{i % 10}"
  ok += 1
end
name = "#{ENV['TMPDIR'] || '/tmp'}/kb_stress_#{Process.pid}.txt"
File.write(name, "hi")
io = File.open(name, "r")
begin; io.set_encoding(Encoding::UTF_16BE); raise "no raise"; rescue ArgumentError; end
begin; io.set_encoding("utf-8", "utf-8".encode(Encoding::UTF_16BE)); raise "no raise2"; rescue ArgumentError; end
io.binmode
begin; io.set_encoding("utf-8", universal_newline: true); raise "no raise3"; rescue ArgumentError; end
io.close; File.unlink(name)
puts "ok=#{ok} ng=0"
