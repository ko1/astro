p (1..10).bsearch { |x| x >= 5 }
p (1..10).bsearch { |x| 5 <=> x }
p (1..10).bsearch { |x| x >= 100 }
def t; yield; rescue TypeError; "TE"; end
p t { (1..10).bsearch { |x| "str" } }
p t { (1..10).bsearch { |x| Object.new } }
p (1.0..10.0).bsearch { |x| x >= 3.5 }
