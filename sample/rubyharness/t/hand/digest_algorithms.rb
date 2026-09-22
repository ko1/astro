# Every message-length case around the block/padding boundaries, for each hash.
require "digest"

INPUTS = ["", "a", "abc", "message digest",
          *[55, 56, 63, 64, 65, 111, 112, 119, 127, 128, 129, 1000].map { |n| "a" * n },
          "\x00\xff\x80".b * 100]

[Digest::MD5, Digest::SHA1, Digest::SHA256, Digest::SHA384, Digest::SHA512].each do |k|
  INPUTS.each { |s| puts "#{k.name} #{s.bytesize} #{k.hexdigest(s)}" }
end

d = Digest::SHA256.new
d << "a"
d.update("bc")
p d.hexdigest
p d.hexdigest                     # #digest is non-destructive
p d.digest.encoding
p [d.digest_length, d.block_length]
p d.dup.hexdigest
d.reset
p d.hexdigest

p Digest::SHA2.new(384).hexdigest("abc")
p Digest::SHA2.new(512).hexdigest("abc")
p Digest::MD5.base64digest("abc")
p Digest::SHA1.hexdigest("abc") == Digest::SHA1.new.update("abc").hexdigest
p Digest.hexencode("\x01\xff".b)
