class TI; def to_int; 1; end; end
p "hello"[TI.new]
p "hello"[TI.new, 2]
p "hello"[TI.new..3]
p "hello".slice(TI.new, 2)
