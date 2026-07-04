# Integer#[] coerces its bit index (and length) via #to_int. vs ruby.
class ToInt; def to_int; 2; end; end
p 0b1011[ToInt.new]
p 0b1011[0]
p 0b1011[1, 2]
p 255[ToInt.new, 3]
p (2**70)[ToInt.new]
p 0b1010[1..2]
