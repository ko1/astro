p((1..10).minmax)
p((1...10).minmax)
p((5..5).minmax)
def t; yield; rescue RangeError; "RE"; rescue ArgumentError; "AE"; end
p(t { (1..).minmax })
p(t { (..5).minmax })
p(('a'..'e').minmax)
