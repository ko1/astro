class WithF; def to_f; 1.5; end; end
p Float(WithF.new)
def t; yield; rescue TypeError; "TE"; end
p t { Float(nil) }
p t { Float(true) }
p t { Float(Object.new) }
p Float(3)
p Float("2.5")
class BadF; def to_f; "nope"; end; end
p t { Float(BadF.new) }
