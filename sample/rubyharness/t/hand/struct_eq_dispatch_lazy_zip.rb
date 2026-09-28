# Struct#== / eql? call each member's own ==  (an Array of structs, a Hash, a user object); Lazy#zip with a block is eager
P = Struct.new(:a, :b)
class Q < Struct.new(:list, :h); end
p P.new(1, [P.new(2, 3)]) == P.new(1, [P.new(2, 3)])
p Q.new([P.new(1, 2)], {k: P.new(3, 4)}) == Q.new([P.new(1, 2)], {k: P.new(3, 4)})
p Q.new([P.new(1, 2)], {}) == Q.new([P.new(1, 9)], {})
p P.new(1, 1.0).eql?(P.new(1, 1)), P.new(1, 1.0) == P.new(1, 1)
p P.new([1], "s").eql?(P.new([1], "s")), P.new([1.0], "s").eql?(P.new([1], "s"))
class Wild; def ==(o) = true; end
p P.new(Wild.new, 1) == P.new(Object.new, 1)
r1 = P.new(1, nil); r1.b = r1; r2 = P.new(1, nil); r2.b = r2; p r1 == r2
D = Data.define(:x, :y); p D.new(x: [D.new(x: 1, y: 2)], y: 3) == D.new(x: [D.new(x: 1, y: 2)], y: 3)
class SS < String; def ==(o) = true; end
p P.new(SS.new("a"), 1) == P.new("b", 1)
out = []
r = [1, 2, 3].each.lazy.zip([4, 5, 6].each.lazy) { |a, b| out << [a, b] }
p r, out
p [1, 2].lazy.zip([3, 4]).to_a, [1, 2].lazy.zip([3, 4]).class
out2 = []; (1..3).lazy.zip(4..6) { |x| out2 << x }; p out2
