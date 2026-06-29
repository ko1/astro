a = [10, 20, 30, 40]
p a.values_at(0, 2)
p a.values_at(-1, 1)
class TI; def to_int; 2; end; end
p a.values_at(TI.new, TI.new)
p a.values_at(1..3)
def t; yield; rescue TypeError; "TE"; end
p t { a.values_at(Object.new) }
