require 'stringio'
s = "test".freeze
io = StringIO.open(s, "rb") { |x| [x.read, x.string.encoding, x.external_encoding] }
p io, s.encoding
io2 = StringIO.new("abc".freeze, "rb"); p io2.read.encoding, io2.string.encoding, io2.string.frozen?
io4 = StringIO.new("abc".freeze); p io4.read.encoding, (io4.write("x") rescue $!.class)
