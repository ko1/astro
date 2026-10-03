# one send site seeing many receiver classes (polymorphic inline cache): results, redefinition,
# singleton receivers, cfuncs, private methods, method_missing, more classes than entries
classes = (0...7).map do |i|
  Class.new do
    define_method(:name_of) { "c#{i}" }
    def twice(x) = [x, x]
    def to_s = "obj"
  end
end
objs = classes.map(&:new)
r = []
3.times { objs.each { |o| r << o.name_of << o.twice(1).size } }
p r.uniq.sort_by(&:to_s)

A = Struct.new(:v) do
  def val = v
end
class B; def val = :b; end
class C; def val = :c; end
site = ->(o) { o.val }
p [A.new(1), B.new, C.new, A.new(2)].map(&site)
class B; def val = :b2; end                      # redefinition reaches the polymorphic site
p [A.new(1), B.new, C.new].map(&site)
s = C.new
def s.val = :single                              # singleton receiver at the same site
p [A.new(3), s, C.new, B.new].map(&site)
p [1, 1.5, "x", :y, [1], {a: 1}, nil].map { _1.to_s }   # cfunc receivers
class D; def method_missing(n, *) = [:mm, n]; def respond_to_missing?(*) = true; end
class E; private def val = :priv; end
p [A.new(4), D.new, C.new].map(&site)
p((begin; site.(E.new); rescue NoMethodError => e; e.message[/private method 'val'/]; end))
x = 0
200.times { |i| x += [A.new(i), B.new, C.new, s].map(&site).size; GC.start if i % 50 == 0 }
p x
