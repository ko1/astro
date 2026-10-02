# obj.is_a?(X) / kind_of?(X): the AOT inline body answers a class / module argument
# itself; everything else (a non-module argument, a user-defined is_a?) as CRuby
module M; end
class A; end
class B < A; include M; end
class Fake; def is_a?(k) = k == String; def kind_of?(k) = :fake; end
vals = [1, 1.5, "s", :sym, nil, true, [1], { a: 1 }, A.new, B.new, Object.new.extend(M), Fake.new, Class, M, BasicObject]
klasses = [Integer, Numeric, Comparable, String, Symbol, NilClass, Array, Hash, Enumerable, A, B, M, Object, Kernel, BasicObject, Module, Class]
3.times do
  vals.each { |v| p klasses.map { |k| v.is_a?(k) } }
  vals.each { |v| p klasses.map { |k| v.kind_of?(k) } }
end
s = +"x"
def s.extra = 1
p s.is_a?(String), s.is_a?(s.singleton_class), B.new.is_a?(B.new.singleton_class)
[1, "x", nil].each do |v|
  v.is_a?(3)
rescue TypeError => e
  p e.message
end
