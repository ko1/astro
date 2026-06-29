p [1, 2].concat([3, 4])
p [1].concat([2], [3, 4])
class TA; def to_ary; [9, 8]; end; end
p [1].concat(TA.new)
p [1].concat([2], TA.new)
a = [1, 2]; a.concat(a); p a
def t; yield; rescue TypeError; "TE"; end
p t { [1].concat(5) }
p [1].concat
