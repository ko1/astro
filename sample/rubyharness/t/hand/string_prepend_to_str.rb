p "world".dup.prepend("hello ")
p "c".dup.prepend("a", "b")
class TS; def to_str; "X"; end; end
p "z".dup.prepend(TS.new)
p "z".dup.prepend("1", TS.new, "2")
def t; yield; rescue TypeError; "TE"; rescue FrozenError; "FE"; end
p t { "z".dup.prepend(5) }
p t { "z".freeze.prepend("a") }
p "".dup.prepend("only")
