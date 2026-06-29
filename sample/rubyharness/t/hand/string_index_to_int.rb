class TI; def to_int; 2; end; end
p "hello".index("l", TI.new)
p "hello".index("l")
p "hello".rindex("l", TI.new)
p "hello".rindex("l")
def t; yield; rescue TypeError; "TE"; end
p t { "x".index("x", Object.new) }
p t { "x".rindex("x", Object.new) }
