# Gzip close / finish lifecycle: the header goes out on the first (even empty) write; a
# second close is a no-op (nil), close after finish leaves the IO open, and finish /
# write / flush / read after either are Zlib::GzipFile::Error.
require 'zlib'; require 'stringio'
def t = (yield rescue [$!.class, $!.message])
io = StringIO.new; gz = Zlib::GzipWriter.new(io); gz.mtime = 1; gz.write(""); p io.string.bytesize
gz.write("x"); p io.string.bytesize; gz.flush; p io.string.bytesize > 10
r = gz.close; p r.class, io.closed?, t { gz.close }, t { gz.closed? }
io2 = StringIO.new; g2 = Zlib::GzipWriter.new(io2); g2.write("abc"); p g2.finish.equal?(io2), io2.closed?, t { g2.close }, io2.closed?, t { g2.finish }
io3 = StringIO.new; g3 = Zlib::GzipWriter.new(io3); p t { g3.finish; g3.finish }
p Zlib::GzipReader.new(StringIO.new(io2.string)).read
g = Zlib::GzipReader.new(StringIO.new(Zlib.gzip("a"))); p g.close.class, t { g.close }, t { g.finish }
g = Zlib::GzipReader.new(StringIO.new(Zlib.gzip("a"))); io = g.finish; p io.class, io.closed?, t { g.close }, t { g.read }
w = Zlib::GzipWriter.new(StringIO.new); p t { w.flush; w.write("a") }; w.close; p t { w.write("b") }, t { w.flush }, t { w.mtime = 3 }, t { w.closed? }
