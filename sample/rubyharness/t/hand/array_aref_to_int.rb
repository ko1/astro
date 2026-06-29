class TI; def to_int; 2; end; end
p [10, 20, 30, 40].at(TI.new)
p [10, 20, 30, 40][TI.new]
p [10, 20, 30, 40][TI.new, TI.new]
p [10, 20, 30][1]
p [10, 20, 30][1, 2]
p [10, 20, 30][-1]
def t; yield; rescue TypeError; "TE"; end
p t { [1, 2].at(Object.new) }
p [1, 2, 3][0..1]
