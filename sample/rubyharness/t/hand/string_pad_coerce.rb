p "hello".rjust(10)
p "hello".rjust(10, "123")
p "hello".ljust(10, "*")
p "hello".center(11, "-")
class TI; def to_int; 8; end; end
p "hi".rjust(TI.new)
class TS; def to_str; "ab"; end; end
p "x".rjust(5, TS.new)
def t; yield; rescue TypeError; "TE"; end
p t { "x".rjust(5, 99) }
