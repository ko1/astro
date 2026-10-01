# is_a? / === with a class target walks only superclasses; modules still see
# include / prepend / extend.  A class can never be included.
class A; end
class B < A; end
module M; end
module N; include M; end
class C < B; include N; end
module P; end
class C; prepend P; end
o = C.new
p [o.is_a?(A), o.is_a?(B), o.is_a?(C), o.is_a?(M), o.is_a?(N), o.is_a?(P), o.is_a?(String)]
p [A === o, String === o, M === o, Comparable === "x", Integer === 3, Float === 3, Numeric === 3.0]
s = Object.new
s.extend(N)
p [s.is_a?(M), s.is_a?(N), s.is_a?(Object), s.is_a?(BasicObject), s.singleton_class === s]
bo = BasicObject.new
p [A === bo, (Class === A), (Module === M), (Class === M)]
begin
  Class.new { include String }
rescue TypeError => e
  p e.class
end
begin
  Object.new.extend(A)
rescue TypeError => e
  p e.class
end
def kind(v)
  case v
  when String then :s
  when Integer then :i
  when Float then :f
  when Array then :a
  when Hash then :h
  when nil then :n
  when Comparable then :cmp
  else :other
  end
end
p ["x", 1, 2.0, [], {}, nil, :sym, 1r, Object.new].map { kind(_1) }
class MyStr < String; end
p [MyStr.new("a").is_a?(String), String === MyStr.new, kind(MyStr.new("q"))]
