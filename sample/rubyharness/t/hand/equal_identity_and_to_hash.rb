# rb_equal: the same object is equal without calling its #==; Hash#== asks a
# #to_hash object back (other == self) and compares Proc values with Proc#==.
class X; def ==(o) = false; end
x = X.new
p [x].index(x), [x].tap { _1.delete(x) }.size, [x].count(x), [x].rindex(x), ({k: x} == {k: x}), {k: x}.value?(x), {k: x}.key(x), [[x, 1]].assoc(x)&.last
class C; def initialize(h) = @h = h; def to_hash = @h; def ==(o) = @h == o; end
p({a: 1} == C.new({a: 1}), {a: 1} == C.new({a: 2}), {a: 1} == Object.new)
l = ->(v) { v }
p({a: l} == {a: l.dup}, {a: l} == {a: ->(v) { v }}, [l] == [l.dup], [method(:p)] == [method(:p)])
p [[1, 2], [3]].include?([1, 2]), {a: [1, :x]} == {a: [1, :x]}, [[1, l]].include?([1, l]), {[1, 2] => 3}.key?([1, 2])
