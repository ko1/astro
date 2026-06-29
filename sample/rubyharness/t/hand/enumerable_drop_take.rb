class C; include Enumerable; def each; [1, :go, 3].each { |v| yield v }; end; end
c = C.new
p c.drop(1)
p c.take(2)
class TI; def to_int; 1; end; end
p c.drop(TI.new)
p c.take(TI.new)
def t; yield; rescue ArgumentError; "AE"; end
p t { c.drop(-1) }
p t { c.take(-1) }
