class TI; def to_int; 1; end; end
a = [10, 20, 30, 40]
p a.dup.slice!(TI.new)
p a.dup.slice!(TI.new, TI.new)
p a.dup.slice!(1)
p a.dup.slice!(1, 2)
p a.dup.slice!(1..2)
def t; yield; rescue TypeError; "TE"; end
p t { [1, 2].slice!(Object.new) }
