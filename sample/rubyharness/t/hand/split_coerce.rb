# String#split coerces its pattern via #to_str and its limit via #to_int. vs ruby.
class ToStr; def to_str; ","; end; end
class ToInt; def to_int; 2; end; end
p "1,2,3".split(ToStr.new)
p "1.2.3.4".split(".", ToInt.new)
p "a b c".split
p "1,2,".split(",", 3)
p "1,2,,,".split(",", -1)
