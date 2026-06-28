class TI; def to_int; 1; end; end
p "hello"[TI.new]
p "hello"[TI.new, 2]
p "hello".slice(TI.new, 2)
s = "hi".dup
s[TI.new] = "X"
p s
