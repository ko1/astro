require 'stringio'
io = StringIO.new
io.puts "line1"
io.puts "line2"
io.print "no-eol"
p io.string
sr = StringIO.new("a\nb\nc\n")
p sr.gets
p sr.gets
p sr.readlines
sr.rewind
p sr.read
p sr.readlines.length
w = StringIO.new
w.write("abc", "def")
w << "ghi" << "jkl"
p w.string
s2 = StringIO.new("x\ny\nz")
p s2.each_line.to_a
lines = []
StringIO.new("p\nq\n").each_line { |l| lines << l.chomp }
p lines
p StringIO.new("hello world").read(5)
