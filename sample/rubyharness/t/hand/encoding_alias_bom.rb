# Encoding.aliases is CRuby's full table (SJIS is Windows-31J, not Shift_JIS),
# String#force_encoding rejects a name no registry knows, and "bom|utf-8" on an
# IO / StringIO consumes a leading BOM.  CSV.open sets bom| for every stream.
require 'stringio'
require 'tmpdir'

%w[UTF8 utf8 utf-8 ASCII8BIT ascii-8bit ISO-8859-1 ISO8859-15 eucjp SJIS PCK
   CP932 CP1252 646 csWindows31J UTF-8-MAC external locale binary].each do |n|
  begin
    puts "#{n}\t#{Encoding.find(n)}"
  rescue => e
    puts "#{n}\t#{e.class}: #{e.message}"
  end
end

%w[CP932 SJIS ISO8859-1 nope UTF8].each do |n|
  begin
    puts "fe #{n}\t#{(+"x").force_encoding(n).encoding}"
  rescue => e
    puts "fe #{n}\t#{e.class}: #{e.message}"
  end
end
p (+"x").force_encoding(Encoding::UTF_16LE).encoding

io = StringIO.new("\xEF\xBB\xBFa,b\n".b.force_encoding("UTF-8"), "r", encoding: "bom|utf-8")
p [io.external_encoding, io.pos, io.read]
io2 = StringIO.new("a,b\n", "r", encoding: "bom|utf-8")
p [io2.external_encoding, io2.pos, io2.read]

Dir.mktmpdir do |d|
  path = File.join(d, "bom.txt")
  File.binwrite(path, "\xEF\xBB\xBFhello\n")
  File.open(path, "r", encoding: "bom|utf-8") { |f| p [f.external_encoding, f.pos, f.read] }
  plain = File.join(d, "plain.txt")
  File.binwrite(plain, "hello\n")
  File.open(plain, "r", encoding: "bom|utf-8") { |f| p [f.external_encoding, f.pos, f.read] }
  # an encoding nobody knows only WARNS on the open path (captured so the two
  # streams cannot interleave differently)
  saved = $stderr
  $stderr = StringIO.new
  File.open(plain, "r", encoding: "nonexistent") { |f| p f.external_encoding }
  msg = $stderr.string
  $stderr = saved
  puts msg.sub(/\A.*:\d+: /, "FILE:LINE: ")
end
