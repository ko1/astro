S = Struct.new(:x)
o = nil; s = S.new
for s&.x in [7, 8]; end; p s.x
for o&.x in [7]; end; p :ok
for *q in [[1, 2], 3]; p q; end
r = for a in 1..3; end; p r, a
h = {a: 1}; for k, v in h; p [k, v]; end
class E; include Enumerable; def each; yield 1; yield; yield 2, 3; end; end
for z in E.new; p z; end
for z1, z2 in E.new; p [z1, z2]; end
