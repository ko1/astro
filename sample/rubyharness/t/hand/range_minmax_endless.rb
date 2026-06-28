p((1..10).min)
p((1..10).max)
p((1..).min)
p((..5).max)
def t; yield; rescue RangeError; "RE"; end
p(t { (1..).max })
p(t { (..5).min })
