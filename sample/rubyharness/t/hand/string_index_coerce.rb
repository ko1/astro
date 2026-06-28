class S; def to_str; "lo"; end; end
class TI; def to_int; 1; end; end
p "hello".index("l")
p "hello".index(S.new)
p (begin; "hi".index(5); rescue TypeError; "TE"; end)
p (begin; "hi".index(TI.new); rescue TypeError; "TE"; end)
p "hello".index("l", 3)
p "hello".index("x")
