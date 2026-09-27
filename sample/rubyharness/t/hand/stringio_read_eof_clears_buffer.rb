# StringIO#read(n, buf) at EOF empties buf (fileutils' compare_stream loops on it otherwise).
require 'stringio'
io = StringIO.new("abc"); buf = +""
p io.read(10, buf), buf, io.read(10, buf), buf
s = StringIO.new(""); b = +"x"
p s.read(0, b), b, s.read(0), s.read(5), s.read, s.read(nil, b), b
