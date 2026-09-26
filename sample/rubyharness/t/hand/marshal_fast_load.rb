# Marshal.load of plain data (decoded in C) must give what the full loader
# gives: back-references, shared strings, encodings, symlinks, hash defaults,
# floats, string keys frozen as Hash#[]= does; anything else falls back.
def rt(o) = Marshal.load(Marshal.dump(o))
s = "shared"
a = [s, s, :sym, :sym, "é", "x".b, "a".encode("US-ASCII"), "ü".encode("ISO-8859-1"), 1.5, 1.5, -0.0, Float::INFINITY, 2**40, -2**40, 123, -129, nil, true, false]
b = rt(a)
p b, b[0].equal?(b[1]), b.map { _1.is_a?(String) ? _1.encoding : nil }.compact
p rt(Float::NAN).nan?, rt(-Float::INFINITY)
h = Hash.new(7); h["k"] = [1, 2]; h[:s] = {"n" => nil}; h[2.5] = :f; h[[1, 2]] = :ary
h2 = rt(h)
p h2, h2.default, h2["zz"], h2.keys.first.frozen?, h2[[1, 2]]
cyc = []; cyc << cyc; c2 = rt(cyc); p c2[0].equal?(c2)
hs = {}; hs[:self] = hs; p rt(hs)[:self].keys
p rt(:"sïm"), rt([:"sïm", :"sïm", :plain, :plain]), rt("").encoding, rt({})
S = Struct.new(:a); p rt(S.new(1)), rt([S.new(2), "x"])            # fallbacks
class Pt; attr_accessor :x; end; pt = Pt.new; pt.x = 3; p rt([pt, pt]).then { [_1[0].x, _1[0].equal?(_1[1])] }
p rt("s".dup.tap { _1.instance_variable_set(:@iv, 1) }).instance_variable_get(:@iv)
p Marshal.load(Marshal.dump([1, "a"]), ->(x) { x.is_a?(Integer) ? x * 10 : x })
p Marshal.load(Marshal.dump(["q", "q"]), freeze: true).map(&:frozen?)
p rt(1..3), rt(/re/i), rt(3r), rt(2**100)
begin; Marshal.load("\x04\x08[\x07i\x06"); rescue => e; p e.class, e.message; end
begin; Marshal.load("\x04\x09i\x06"); rescue => e; p e.class; end
p Marshal.load(Marshal.dump("tail") + "garbage")
