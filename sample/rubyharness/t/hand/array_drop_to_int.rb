class TI; def to_int; 2; end; end
p [1, 2, 3, 4].drop(TI.new)
p [1, 2, 3, 4].drop(1)
p [1, 2, 3].drop(0)
def t; yield; rescue TypeError; "TE"; rescue ArgumentError; "AE"; end
p t { [1, 2].drop(Object.new) }
p t { [1, 2].drop(-1) }
