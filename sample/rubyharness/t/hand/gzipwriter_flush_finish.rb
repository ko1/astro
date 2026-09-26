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
# an IO that raises on the first write (a client that went away): the data stays in zlib and
# #finish sends the rest and the trailer in one write, byte for byte as CRuby
class Once; attr_reader :chunks; def initialize = (@chunks = []); def write(d) = (@chunks << d.unpack1("H*"); raise "gone" if @chunks.size == 1; d.bytesize); end
o = Once.new; g = Zlib::GzipWriter.new(o)
begin; g.write("foo"); g.flush; rescue => e; p e.message; ensure; g.finish; end
p o.chunks.size, o.chunks[1], Zlib::Inflate.new(32 + Zlib::MAX_WBITS).inflate(o.chunks.map { [_1].pack("H*") }.join)
s = StringIO.new; g = Zlib::GzipWriter.new(s); g.write("a"); g.finish; p (Time.now.to_i - s.string[4, 4].unpack1("V")).abs < 5; s = StringIO.new; g = Zlib::GzipWriter.new(s); g.mtime = 0; g.write("a"); g.finish; p s.string[4, 4].unpack1("V")
