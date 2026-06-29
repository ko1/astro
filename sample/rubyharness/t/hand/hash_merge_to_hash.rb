class TH; def to_hash; { x: 9, y: 8 }; end; end
p({ a: 1 }.merge(TH.new))
p({ a: 1, x: 0 }.merge(TH.new) { |k, o, n| o + n })
h = { a: 1 }
h.merge!(TH.new)
p h
p({ a: 1 }.merge({ b: 2 }, { c: 3 }))
def t; yield; rescue => e; e.class; end
p t { { a: 1 }.merge(5) }
class HS < Hash; end
hs = HS.new
hs[:z] = 7
p({ a: 1 }.merge(hs))
