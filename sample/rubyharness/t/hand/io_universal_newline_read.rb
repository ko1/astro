# Universal newline on read ("rt", newline: :universal, universal_newline: true, textmode:):
# text readers see \r\n and \r as \n; byte readers stay raw.  (csv; w12 A14.  Cases from
# trials/2026-09-19-gem-dropin/w12min/A/14b_universal_newline_cases.rb, read side only.)
require "fileutils"
require "tmpdir"; require "fileutils"
DIR = Dir.mktmpdir("unl"); at_exit { FileUtils.rm_rf(DIR) }
FileUtils.mkdir_p(DIR)
PATH = File.join(DIR, "t14b.tmp")
RAW = "a\r\nb\rc\nd\r\r\ne\r"          # CRLF, lone CR, LF, CR CR LF, trailing CR
File.binwrite(PATH, RAW)

def show(label)
  r = begin
    yield
  rescue Exception => e
    "#{e.class}: #{e.message}"
  end
  puts "#{label}: #{r.inspect}"
end

puts "== which open spellings turn universal newline on (read side)"
[["rt", {}], ["r", {newline: :universal}], ["r", {universal_newline: true}], ["r", {textmode: true}],
 ["r:UTF-8", {newline: :universal}], ["r+t", {}],
 ["r", {}], ["rb", {}], ["r", {newline: :crlf}], ["r", {newline: :lf}], ["r", {newline: :cr}],
 ["r", {crlf_newline: true}], ["r", {universal_newline: false}], ["r", {newline: nil}],
 ["rt", {newline: :lf}], ["rt", {universal_newline: false}], ["rt", {newline: nil}],
 ["r:Shift_JIS:UTF-8", {}],
 ["rt", {binmode: true}], ["rb", {newline: :universal}], ["rb", {universal_newline: true}],
 ["rb", {universal_newline: false}], ["r", {newline: :bogus}]].each do |m, kw|
  show("#{m} #{kw}") { File.open(PATH, m, **kw) { |f| [f.read, f.external_encoding, f.binmode?] } }
end
show("mode: kw rt") { File.open(PATH, mode: "rt") { |f| f.read } }
show("File.new rt") { f = File.new(PATH, "rt"); x = f.read; f.close; x }
show("Kernel#open rt") { open(PATH, "rt") { |f| f.read } }

puts "== converting readers"
show("read") { File.open(PATH, "rt") { |f| f.read } }
show("read(nil)") { File.open(PATH, "rt") { |f| f.read(nil) } }
show("read(nil, buf)") { File.open(PATH, "rt") { |f| b = +"zz"; f.read(nil, b); b } }
show("gets loop") { File.open(PATH, "rt") { |f| a = []; while (l = f.gets); a << l; end; a } }
show("gets(nil)") { File.open(PATH, "rt") { |f| f.gets(nil) } }
show("gets('')") { File.open(PATH, "rt") { |f| [f.gets(""), f.gets("")] } }
show("gets(\"\\r\")") { File.open(PATH, "rt") { |f| f.gets("\r") } }
show("gets(\"\\r\\n\")") { File.open(PATH, "rt") { |f| f.gets("\r\n") } }
show("gets(2) x4") { File.open(PATH, "rt") { |f| Array.new(4) { f.gets(2) } } }
show("gets(nil, 3) x5") { File.open(PATH, "rt") { |f| Array.new(5) { f.gets(nil, 3) } } }
show("gets chomp") { File.open(PATH, "rt") { |f| Array.new(7) { f.gets(chomp: true) } } }
show("readline x2") { File.open(PATH, "rt") { |f| [f.readline, f.readline(chomp: true)] } }
show("readlines") { File.open(PATH, "rt") { |f| f.readlines } }
show("readlines chomp") { File.open(PATH, "rt") { |f| f.readlines(chomp: true) } }
show("readlines(\"\\r\\n\")") { File.open(PATH, "rt") { |f| f.readlines("\r\n") } }
show("each_line") { File.open(PATH, "rt") { |f| a = []; f.each_line { |l| a << l }; a } }
show("each_line enum") { File.open(PATH, "rt") { |f| f.each_line.to_a } }
show("each_line(2)") { File.open(PATH, "rt") { |f| f.each_line(2).to_a } }
show("getc loop") { File.open(PATH, "rt") { |f| a = []; while (c = f.getc); a << c; end; a } }
show("readchar x3") { File.open(PATH, "rt") { |f| Array.new(3) { f.readchar } } }
show("each_char") { File.open(PATH, "rt") { |f| f.each_char.to_a } }
show("each_codepoint") { File.open(PATH, "rt") { |f| f.each_codepoint.to_a } }
show("lineno after gets") { File.open(PATH, "rt") { |f| f.gets; f.gets; f.lineno } }

puts "== byte readers stay raw"
show("read(3) x4") { File.open(PATH, "rt") { |f| Array.new(4) { f.read(3) } } }
show("read(100)") { File.open(PATH, "rt") { |f| f.read(100) } }
show("read(3, buf)") { File.open(PATH, "rt") { |f| b = +""; f.read(3, b); b } }
show("getbyte loop") { File.open(PATH, "rt") { |f| a = []; while (b = f.getbyte); a << b; end; a } }
show("each_byte") { File.open(PATH, "rt") { |f| f.each_byte.to_a } }
show("readbyte x2") { File.open(PATH, "rt") { |f| [f.readbyte, f.readbyte] } }
show("readpartial") { File.open(PATH, "rt") { |f| f.readpartial(100) } }
show("read_nonblock") { File.open(PATH, "rt") { |f| f.read_nonblock(100) } }
show("sysread") { File.open(PATH, "rt") { |f| f.sysread(100) } }
show("pread") { File.open(PATH, "rt") { |f| f.pread(100, 0) } }

puts "== state changes"
show("ungetc CRLF not converted") { File.open(PATH, "rt") { |f| f.ungetc("\r\n"); f.read } }
show("gets then ungetc") { File.open(PATH, "rt") { |f| l = f.gets; f.ungetc("\r"); [l, f.read] } }
show("seek(1) read") { File.open(PATH, "rt") { |f| f.seek(1); f.read } }
show("pos=2 read") { File.open(PATH, "rt") { |f| f.pos = 2; f.read } }
show("read then rewind read") { File.open(PATH, "rt") { |f| a = f.read; f.rewind; [a, f.read] } }
show("read(8) then eof?/read") { File.open(PATH, "rt") { |f| f.read(8); [f.eof?, f.read] } }
show("binmode then read") { File.open(PATH, "rt") { |f| f.binmode; [f.read, f.binmode?] } }
show("set_encoding keeps rt") { File.open(PATH, "rt") { |f| f.set_encoding("UTF-8"); f.read } }
show("set_encoding(nil) keeps rt") { File.open(PATH, "rt") { |f| f.set_encoding(nil); f.read } }
show("set_encoding universal_newline:") { File.open(PATH) { |f| f.set_encoding("UTF-8", universal_newline: true); f.read } }
show("reopen rt") { File.open(PATH) { |f| f.reopen(PATH, "rt"); f.read } }
show("pos at EOF") { File.open(PATH, "rt") { |f| f.read; f.pos } }

puts "== class-level readers"
show("File.read mode rt") { File.read(PATH, mode: "rt") }
show("File.read newline") { File.read(PATH, newline: :universal) }
show("File.read universal_newline") { File.read(PATH, universal_newline: true) }
show("File.read textmode") { File.read(PATH, textmode: true) }
show("File.read mode r") { File.read(PATH, mode: "r") }
show("File.read len raw") { File.read(PATH, 3, mode: "rt") }
show("File.read len off raw") { File.read(PATH, 4, 1, mode: "rt") }
show("IO.read rt") { IO.read(PATH, mode: "rt") }
show("File.read open_args") { File.read(PATH, open_args: ["rt"]) }
show("File.readlines rt") { File.readlines(PATH, mode: "rt") }
show("File.readlines unl chomp") { File.readlines(PATH, universal_newline: true, chomp: true) }
show("IO.readlines newline") { IO.readlines(PATH, newline: :universal) }
show("IO.readlines sep CRLF") { IO.readlines(PATH, "\r\n", mode: "rt") }
show("IO.readlines limit") { IO.readlines(PATH, "\n", 2, mode: "rt") }
show("IO.readlines paragraph") { IO.readlines(PATH, "", mode: "rt") }
show("File.foreach rt") { File.foreach(PATH, mode: "rt").to_a }
show("File.foreach newline block") { a = []; File.foreach(PATH, newline: :universal) { |l| a << l }; a }
show("IO.foreach chomp") { IO.foreach(PATH, mode: "rt", chomp: true).to_a }

puts "== IO.new / IO.open / for_fd / popen"
show("IO.new rt") { fd = IO.sysopen(PATH); io = IO.new(fd, "rt"); x = io.read; io.close; x }
show("IO.new mode: rt") { fd = IO.sysopen(PATH); io = IO.new(fd, mode: "rt"); x = io.read; io.close; x }
show("IO.new r + newline") { fd = IO.sysopen(PATH); io = IO.new(fd, "r", newline: :universal); x = io.read; io.close; x }
# CRuby quirk: without an explicit mode, io_initialize replaces fmode by the fd's
# own flags and drops FMODE_TEXTMODE, so the newline option is ignored.
show("IO.open no mode + newline (raw!)") { fd = IO.sysopen(PATH); IO.open(fd, newline: :universal) { |io| io.read } }
show("IO.for_fd no mode + unl (raw!)") { fd = IO.sysopen(PATH); io = IO.for_fd(fd, universal_newline: true); x = io.read; io.close; x }
show("popen rt") { IO.popen(["cat", PATH], "rt") { |io| io.read } }
show("popen newline") { IO.popen(["cat", PATH], newline: :universal) { |io| io.readlines } }
show("popen plain") { IO.popen(["cat", PATH]) { |io| io.read } }

puts "== buffer boundaries"
big = ("a" * 8191 + "\r\n") * 9 + ("b" * 65535 + "\r\n") * 3 + ("c" * 8191 + "\r" + "x") * 3 +
      ("d" * 8190 + "\r\r\n") * 3 + ("e" * 8191 + "\r")
BIG = File.join(DIR, "t14b_big.tmp")
File.binwrite(BIG, big)
want = big.gsub(/\r\n?/, "\n")
show("big read ok") { t = File.read(BIG, mode: "rt"); [t.bytesize, t.count("\r"), t == want] }
show("big File.open read ok") { File.open(BIG, "rt") { |f| t = f.read; [t.bytesize, t == want] } }
show("big gets sizes") { File.open(BIG, "rt") { |f| a = []; while (l = f.gets); a << l.bytesize; end; a.tally } }
show("big readlines == split") { File.readlines(BIG, mode: "rt") == want.lines }
show("big getc count") { File.open(BIG, "rt") { |f| n = 0; r = 0; while (c = f.getc); n += 1; r += 1 if c == "\r"; end; [n, r] } }
show("big gets(nil, 4096) joined") { File.open(BIG, "rt") { |f| s = +""; while (x = f.gets(nil, 4096)); s << x; end; s == want } }
show("big each_char joined") { File.open(BIG, "rt") { |f| f.each_char.to_a.join == want } }

puts "== CR arriving in a separate write on a pipe"
show("pipe gets x3") do
  r, w = IO.pipe
  rt = IO.new(r.fileno, "rt", autoclose: false)
  th = Thread.new { w.write "x\r"; w.flush; sleep 0.2; w.write "\ny\r"; w.flush; sleep 0.2; w.write "z"; w.close }
  a = [rt.gets, rt.gets, rt.gets, rt.gets]
  th.join; r.close
  a
end
show("pipe read all") do
  r, w = IO.pipe
  rt = IO.new(r.fileno, "rt", autoclose: false)
  th = Thread.new { w.write "x\r"; w.flush; sleep 0.2; w.write "\ny\r"; w.close }
  a = rt.read
  th.join; r.close
  a
end
show("pipe getc") do
  r, w = IO.pipe
  rt = IO.new(r.fileno, "rt", autoclose: false)
  th = Thread.new { w.write "x\r"; w.flush; sleep 0.2; w.write "\nq"; w.close }
  a = []; while (c = rt.getc); a << c; end
  th.join; r.close
  a
end

puts "== transcoding streams"
U16 = File.join(DIR, "t14b_u16.tmp")
File.binwrite(U16, "a\r\nb\rc\n".encode("UTF-16LE"))
show("rt:UTF-16LE:UTF-8 read") { File.read(U16, mode: "rt:UTF-16LE:UTF-8") }
show("r:UTF-16LE:UTF-8 read (no conv)") { File.read(U16, mode: "r:UTF-16LE:UTF-8") }
SJ = File.join(DIR, "t14b_sjis.tmp")
File.binwrite(SJ, "\u3042\r\n\u3044\r".encode("Shift_JIS"))
show("rt:Shift_JIS:UTF-8 read") { File.read(SJ, mode: "rt:Shift_JIS:UTF-8") }
show("rt:Shift_JIS:UTF-8 gets") { File.open(SJ, "rt:Shift_JIS:UTF-8") { |f| [f.gets, f.gets, f.gets] } }
show("r:Shift_JIS:UTF-8 read (no conv)") { File.read(SJ, mode: "r:Shift_JIS:UTF-8") }
show("rt:Shift_JIS read") { r = File.read(SJ, mode: "rt:Shift_JIS"); [r.encoding, r.bytes] }

