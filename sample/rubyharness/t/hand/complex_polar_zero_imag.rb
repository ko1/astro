# Complex.polar accepts a Complex arg whose imaginary part is 0 (treated as real). vs ruby.
a = Complex.polar(1.0 + 0.0i, Math::PI / 2 + 0.0i)
p a.real.round(10); p a.imag.round(10)
p a.real.real?; p a.imag.real?
p Complex.polar(2.0, 0.0)
p Complex.polar(3)
p Complex.polar(5 + 0i)
begin; Complex.polar(1 + 2i); rescue => e; p e.class; end
begin; Complex.polar(1, 2 + 3i); rescue => e; p e.class; end
