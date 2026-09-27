# Set#== / Range#== of user objects go through their #eql? / #== (the == operator too).
class E; attr_reader :s; def initialize(s) = @s = s; def eql?(o) = s == o.s; def hash = s.hash; end
p Set[E.new(1)] == Set[E.new(1)], Set[E.new(1)] == Set[E.new(2)], Set[1, "x"] == Set[1, "x"]
class V; attr_reader :v; def initialize(v) = @v = v; def ==(o) = o.is_a?(V) && v == o.v; def <=>(o) = v <=> o.v; end
p((V.new(1)..V.new(2)) == (V.new(1)..V.new(2)), (V.new(1)..V.new(2)).eql?(V.new(1)..V.new(2)), (1..2) == (1..2), (1..2) == (1...2))
