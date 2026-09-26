# An anonymous `*` in a block is a rest: define_method / lambda accept any count.
class C
  define_method(:a) { |*| :ok }
  define_method(:b) { |*r| r }
  define_method(:c) { |x, *| x }
  define_method(:d) { |*, k: 1| k }
end
p [:a, :b, :c, :d].map { C.instance_method(_1).arity }, C.instance_method(:a).parameters, C.instance_method(:c).parameters
p C.new.a(1, 2), C.new.b(1, 2), C.new.c(1, 2), C.new.d(1, k: 2)
p proc { |*| }.parameters, lambda { |*| }.parameters, proc { |*| }.arity
l = lambda { |*| :l }; p l.(1, 2), lambda { |a, *| a }.(1, 2, 3), (lambda { |a, | a }.(1, 2) rescue $!.class), [[1, 2]].map(&lambda { |*| :m })
