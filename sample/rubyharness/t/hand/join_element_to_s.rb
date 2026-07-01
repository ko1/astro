# Array#join renders each element via #to_s, dispatching a user object's custom
# #to_s (nested arrays flattened, recursive arrays raise ArgumentError). vs ruby.
class P; def initialize(n); @n = n; end; def to_s; "P#{@n}"; end; end
p [P.new(1), P.new(2), P.new(3)].join(",")
p [P.new(1), P.new(2)].join
p [1, P.new(9), "x", :sym].join("-")
p [[P.new(1)], [P.new(2), P.new(3)]].join(",")
p [1, [2, [P.new(3)]]].join("/")
p [].join(",")
p [P.new(5)].join
p [1, 2, 3].join
p ["a", "b"].join("")
p [nil, true, 1.5].join(",")
p [P.new(1)].join == "P1"
a = [1, 2]; a << a
begin; a.join(","); rescue ArgumentError => e; p e.message; end
