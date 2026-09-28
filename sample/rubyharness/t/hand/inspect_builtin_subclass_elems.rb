class S < Array
  def inspect = "s(#{map(&:inspect).join(", ")})"
end
x = S.new([1, 2])
p "%p" % [x], format("%p", x), "#{x.inspect}", [x].inspect
class H < Hash; def inspect = "H!"; end
p "%p" % [H.new], format("<%p>", "str"), "%-6p|" % [:a]
class S < Array
  def inspect = "q" + super[1..]
end
x = S.new([1, S.new([2])])
p x.inspect, [x, {k: x}].inspect, "%p" % [x]
class T < String; def inspect = "T!"; end
p [T.new("a")], {T.new("b") => 1}
Pt1 = Struct.new(:a); p Pt1.new(S.new([3]))
class U < Array; end
p [U.new([1])], "%p" % [U.new([2])]
O = Object.new
def O.inspect = "OO"
Pt = Struct.new(:a, :b)
p Pt.new(O, [O])
D = Data.define(:x)
p D.new(O)
class A; def initialize(i) = @i = i; def inspect = ("x" * 50 + @i.to_s + [1, 2, 3].map(&:to_s).join)[-6..]; end
S5 = Struct.new(:a, :b, :c, :d, :e)
100.times { |j| s = S5.new(*Array.new(5) { |i| A.new(i + j) }); r = s.inspect; raise r unless r.include?("e=") }
p S5.new(*Array.new(5) { |i| A.new(i) })
class Q < Array; def inspect = "Q" + ("y" * 30).size.to_s; end
p [Q.new, {k: Q.new}, S5.new(Q.new)]
