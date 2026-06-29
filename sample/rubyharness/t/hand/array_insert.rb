a = [1, 2, 3]
p a.insert(1, :x)
class TI; def to_int; 1; end; end
p [1, 2, 3].insert(TI.new, :y)
def t; yield; rescue IndexError; "IE"; rescue TypeError; "TE"; end
p t { [1, 2].insert(-5, :z) }
p t { [1, 2].insert(Object.new, :w) }
p [1, 2].insert(5, :pad)
