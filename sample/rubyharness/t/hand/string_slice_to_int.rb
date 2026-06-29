class TI; def to_int; 2; end; end
p "hello".slice(TI.new)
p "hello".slice(TI.new, TI.new)
p "hello".slice(1, TI.new)
p "hello".slice(1, 2)
s = "hello".dup
p s.slice!(TI.new, TI.new)
p s
def t; yield; rescue TypeError; "TE"; end
p t { "hi".slice(0, Object.new) }
