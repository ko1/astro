# Enumerable#take/drop/each_slice/each_cons coerce n via #to_int (TypeError, not
# NoMethodError, for a non-coercible n). vs ruby.
class E
  include Enumerable
  def each; yield 1; yield 2; yield 3; yield 4; end
end
class TI; def to_int; 2; end; end
e = E.new
p e.drop(TI.new)
p e.take(TI.new)
p e.each_slice(TI.new).to_a
p e.each_cons(TI.new).to_a
begin; e.drop("x"); rescue => err; p err.class; end
begin; e.take(Object.new); rescue => err; p err.class; end
begin; e.each_slice([]); rescue => err; p err.class; end
p e.drop(2); p e.take(2)
