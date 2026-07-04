# Multiple assignment coerces a non-Array RHS via #to_ary (Array spreads; a
# non-Array to_ary raises TypeError; swaps still work). vs ruby.
class ToAry; def to_ary; [1, 2]; end; end
a, b, c = ToAry.new
p [a, b, c]
x, *y = ToAry.new
p [x, y]
*p1, q = ToAry.new
p [p1, q]
class BadAry; def to_ary; 5; end; end
begin; a, b = BadAry.new; rescue TypeError; p :type; end
class NoAry; end
d, e = NoAry.new
p e
n, m = 1, 2; n, m = m, n; p [n, m]
