# Array#pack with Fixnum runs (the fast path) mixed with values that need the general one
# (Bignum, Float, #to_int, nil), every integer width and byte order.  Run under both and diff.
class ToI; def to_int = 66; end
xs = [0, 1, 255, 256, -1, -129, 0x12345678, -0x12345678, 2**40 + 5]
%w[C* c* n* v* N* V* S* s* L* l* Q* q* J* S>* L<* q>* C3 N2 NX nX xC* xN2].each do |t|
  p [t, (xs.pack(t) rescue $!.class).then { |r| r.is_a?(String) ? r.unpack1("H*") : r }]
end
p [1, 2**70].pack("Q*") rescue p $!.class
p [1, 2.7, ToI.new].pack("C*").bytes, [1, 2.7, ToI.new].pack("N*").unpack("N*")
p([1, nil].pack("C*")) rescue p $!.class
p [1, 2].pack("C*", buffer: +"ab").bytes
px = Array.new(1000) { |i| i * 2654435761 % 2**32 }
p px.pack("N*").unpack("N*") == px, px.pack("V*").unpack("V*") == px, px.pack("NX" * px.size).bytesize
