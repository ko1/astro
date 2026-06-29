class TS; def to_str; "l"; end; end
class TI; def to_int; 3; end; end
p "hello".byteindex("l")
p "hello".byteindex(TS.new)
p "hello".byteindex("l", TI.new)
p "hello".byterindex("l")
p "hello".byterindex(TS.new)
p "hello".byterindex("l", TI.new)
p "hello".byteindex("l", 3)
def t; yield; rescue TypeError; "TE"; end
p t { "x".byteindex("x", Object.new) }
