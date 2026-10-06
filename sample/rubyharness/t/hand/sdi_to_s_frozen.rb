# AOT inline String#to_s (self for a String itself) and Kernel#frozen? (flags; a chilled literal is not frozen):
# subclasses, singletons, frozen / chilled / immediates, and redefinitions still dispatch
class SubT < String; def to_s = "sub:#{super}"; end
o = Object.new; fo = Object.new.freeze; t = SubT.new("q"); sg = +"sg"
def sg.to_s = "single"
def probe(s, x) = [s.to_s, s.to_s.equal?(s), s.frozen?, x.frozen?]
r = []
3.times { r << probe("lit", o); r << probe(+"mut", fo); r << probe("f".freeze, 1); r << probe(t, :sym) }
r << probe(sg, nil) << probe(String.new("n"), [].freeze) << probe("c#{1}", {}.freeze)
class String; def to_s = "redef"; end
class Object; def frozen? = :ff; end
2.times { r << probe("x", o) }
p r
