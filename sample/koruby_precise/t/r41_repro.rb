# GC-stress repro for the Ruby 4.1 additions: pack/unpack 'r'/'R'/'^' and
# autoload_relative.  Every step allocates (Bignum results, fresh Strings) so a
# stale raw string pointer or an unrooted VALUE shows up as a crash.
ok = 0
vals = [0, 1, -1, 127, -127, 128, -128, 0x3fff, -0x3fff, 0x4000, -0x4000,
        2**64 - 1, -(2**64 - 1), 2**96 - 1, -(2**96 - 1), 63, 64, -63, -64, -65]
200.times do
  s = vals.pack("r*")
  raise "r roundtrip" unless s.unpack("r*") == vals
  u = vals.reject { |v| v < 0 }
  raise "R roundtrip" unless u.pack("R*").unpack("R*") == u
  ok += 1
end
raise "pack loop" unless ok == 200

# the buffer: form grows a String in place while packing
buf = +""
100.times { |i| [i, i * 100000, 2**70 + i].pack("R*", buffer: buf) }
raise "buffer" unless buf.bytesize > 0

# '^' between decodes that allocate Strings
100.times do
  raise "caret" unless "foo   ".unpack("A*^") == ["foo", 6]
  raise "caret2" unless "\x01\x02\x03\x04".unpack("C3X2^") == [1, 2, 3, 1]
end

# incomplete / nil padding
raise "nil pad" unless ([256].pack("R") + "\xFF".b).unpack("RRRR") == [256, nil, nil, nil]
raise "star skip" unless "\xFF".b.unpack("R*") == []

# #to_int coercion allocates and can move the array being packed
class ToI
  def initialize(n) = @n = n
  def to_int = @n * (2**80) + "x".dup.size
end
50.times { |i| raise "to_int" if [ToI.new(i), 1, 2].pack("R*").bytesize < 3 }

# autoload_relative: the path is expanded AT REGISTRATION against this file's
# directory, and the constant then loads it.
module R41AL; end
R41AL.autoload_relative :Leaf, "./sub/../r41_repro_autoload.rb"
p0 = R41AL.autoload?(:Leaf)
raise "autoload? #{p0.inspect}" unless p0 == File.expand_path("r41_repro_autoload.rb", __dir__)
raise "autoload load" unless R41AL::Leaf.v == 42
begin
  eval('autoload_relative :R41Nope, "x.rb"')
  raise "expected LoadError"
rescue LoadError => e
  raise "msg #{e.message}" unless e.message.include?("autoload_relative called without file context")
end
puts "r41_repro ok"
