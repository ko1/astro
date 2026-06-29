p((1..10).size)
p((1...10).size)
p((1..).size)
p(("a"..).size)
p((1..5).size)
def t; yield; rescue TypeError; "TE"; end
p t { (1.0..).size }
