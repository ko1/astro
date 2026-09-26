# GzipWriter streams: #flush sends the data so far, #finish ends the stream
# without closing the IO, #close closes it; the output inflates back.
require 'zlib'; require 'stringio'
class Sink; attr_reader :n, :buf, :closed; def initialize = (@n = 0; @buf = +"".b); def write(d) = (@n += 1; @buf << d; d.bytesize); def close = (@closed = true); end
s = Sink.new; gz = Zlib::GzipWriter.new(s)
gz.write("foo"); gz.flush; p s.n > 0, s.closed
gz.write("bar" * 1000); r = gz.finish; p r.equal?(s), s.closed.nil?
p Zlib::GzipReader.new(StringIO.new(s.buf)).read.size
io = StringIO.new; w = Zlib::GzipWriter.new(io); w << "x" << "y"; w.close; p io.closed?, Zlib.gunzip(io.string)
io2 = StringIO.new; Zlib::GzipWriter.wrap(io2) { |g| g.puts "hi"; begin; g.mtime = 0; rescue => e; p e.class; end }; p Zlib.gunzip(io2.string)
p Zlib.gunzip(Zlib.gzip("abc" * 50)).size
