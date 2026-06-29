class TI; def to_int; 16; end; end
p "ff".to_i(TI.new)
p "101".to_i(2)
p "123".to_i
def t; yield; rescue TypeError; "TE"; end
p t { "5".to_i(Object.new) }
