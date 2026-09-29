# Zlib::Deflate.new honours mem_level / strategy, and #params calls deflateParams (hexapdf
# passes mem_level 6; its PDF came out 132 bytes larger).  CRuby's #params after a #deflate
# that returned its output has no buffer to flush into: StreamError when a flush is needed.
# Run under CRuby and koruby and diff.
require "zlib"
data = (("hello world " * 50) + (0..2000).map { (_1 * 7919 % 256).chr }.join).b * 20
[[9, 15, 6, 0], [9, 15, 9, 0], [1, 15, 1, Zlib::FILTERED], [6, 15, 8, Zlib::HUFFMAN_ONLY], [9, -15, 6, Zlib::RLE]].each do |l, w, m, s|
  d = Zlib::Deflate.new(l, w, m, s)
  out = +""
  data.scan(/.{1,777}/m) { out << d.deflate(_1) }
  out << d.finish; d.close
  p [l, w, m, s, out.bytesize, Zlib.crc32(out)]
end
z = Zlib::Deflate.new; z << "foo"; z.params(Zlib::DEFAULT_COMPRESSION, Zlib::DEFAULT_STRATEGY); z << "bar"; s = z.finish
p s.bytesize, Zlib::Inflate.inflate(s)
z = Zlib::Deflate.new; z << "foo" * 1000; z.params(Zlib::BEST_SPEED, Zlib::FILTERED); z << "bar" * 1000; s = z.finish
p s.bytesize, Zlib::Inflate.inflate(s).size
z = Zlib::Deflate.new(9); z.deflate("x" * 1000)
p((z.params(1, 0) rescue $!.class))
# (the same with an unchanged level SEGVs CRuby 4.1.0dev: rb_deflate_params reads the nil buffer)
