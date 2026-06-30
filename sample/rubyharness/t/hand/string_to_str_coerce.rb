class Str2
  def initialize(s); @s = s; end
  def to_str; @s; end
end
p "hello " + Str2.new("world")
p ["a", "b", "c"].join(Str2.new("-"))
s = "x"; s.concat(Str2.new("yz")); p s
t = "hi"; t << Str2.new("\!"); p t
p "ab".concat("c", Str2.new("d"), "e")
p [1, 2, 3].join(Str2.new(", "))
p ["a", "b"] * Str2.new("|")
p "n".prepend(Str2.new("pre"))
p ["a", "b"] * "+"
p [1, 2, 3] * 2
p "ab" * 3
begin; "a" + 5; rescue TypeError => e; p e.message; end
begin; ["x"].join(5); rescue TypeError => e; p e.message; end
p [1, [2, 3], 4].join("-")
