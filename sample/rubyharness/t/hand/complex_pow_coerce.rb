# Complex#** with a coercible Object: a, b = obj.coerce(self); a ** b. vs ruby.
class C; def coerce(o); [2, 5]; end; end
p(Complex(3, 9) ** C.new)
p(Complex(2, 0) ** 3)
p(Complex(1, 1) ** 2)
p(Complex(2, 0) ** -1)
begin; Complex(1, 2) ** Object.new; rescue => e; p e.class; end
