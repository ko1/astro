p [1, 2, 3, 4].rotate
p [1, 2, 3, 4].rotate(2)
p [1, 2, 3, 4].rotate(-1)
class TI; def to_int; 2; end; end
p [1, 2, 3, 4].rotate(TI.new)
p [1, 2, 3, 4].dup.rotate!(TI.new)
def t; yield; rescue TypeError; "TE"; end
p t { [1, 2].rotate(Object.new) }
