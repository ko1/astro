class TH; def to_hash; { x: 1, y: 2 }; end; end
h = { a: 9 }
h.replace(TH.new)
p h
h2 = { a: 1, b: 2 }
h2.replace({ c: 3 })
p h2
def t; yield; rescue => e; e.class; end
p t { ({}).replace(5) }
class HS < Hash; end
hs = HS.new; hs[:z] = 7
h3 = { a: 1 }
h3.replace(hs)
p h3
