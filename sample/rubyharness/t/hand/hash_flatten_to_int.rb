p({ a: 1, b: 2 }.flatten)
p({ a: [1, 2] }.flatten(2))
class TI; def to_int; 2; end; end
p({ a: [1, 2] }.flatten(TI.new))
def t; yield; rescue TypeError; "TE"; end
p t { { a: 1 }.flatten("x") }
p t { { a: 1 }.flatten(Object.new) }
