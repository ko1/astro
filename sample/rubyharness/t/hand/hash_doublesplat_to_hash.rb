class TH; def to_hash; {x: 1, y: 2}; end; end
p({**TH.new})
p({**TH.new, z: 3})
p({a: 0, **TH.new})
def kw(**opts); opts; end
p kw(**TH.new)
p({**{a: 1}, **{b: 2}})
p({**{}})
def t; yield; rescue TypeError; "TE"; end
p t { {**Object.new} }
p t { {**42} }
