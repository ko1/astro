a = [1, 2, 3]
a.replace([4, 5])
p a
class TA; def to_ary; [9, 8, 7]; end; end
b = [1]
b.replace(TA.new)
p b
def t; yield; rescue TypeError; "TE"; end
p t { [1].replace(5) }
c = [1, 2]; c.replace([]); p c
