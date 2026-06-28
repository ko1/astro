class X; def to_c; Complex(2, 3); end; end
p Complex(X.new)
p Complex(1, 2)
p Complex(X.new) + Complex(1, 1)
