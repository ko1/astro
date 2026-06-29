p "abc".insert(1, "X")
p "abc".insert(-1, "Z")
class TS; def to_str; "QQ"; end; end
p "abc".insert(0, TS.new)
class TI; def to_int; 2; end; end
p "abcd".insert(TI.new, "_")
def t; yield; rescue TypeError; "TE"; rescue IndexError; "IE"; end
p t { "abc".insert(0, 5) }
p t { "abc".insert(99, "x") }
