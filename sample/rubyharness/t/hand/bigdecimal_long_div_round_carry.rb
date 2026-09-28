require "bigdecimal"
a = BigDecimal(101626750278308294097821594550048159, 0); b = BigDecimal(10**35, 0)
p a, b, a.div(b, 20), a.div(b, 36), a.div(b, 40)
p BigDecimal(Rational(101626750278308294097821594550048159, 10**35), 20)
p BigDecimal(0.9999979537560903, 5), BigDecimal("9.99996").round(4), BigDecimal("0.99995").round(4), BigDecimal("-99.9999").round(2), BigDecimal("99.5").round, BigDecimal("9999.99", 5)
p BigDecimal("1.23456", 3), BigDecimal("123456789", 2), BigDecimal(" 1.5 ", 1)
r = Rational(101626750278308294097821594550048159, 10**35); p (BigDecimal("-0.2") / r).round(20), (BigDecimal("-0.2") + r).round(20)
require "bigdecimal"; require "bigdecimal/util"
I = BigDecimal::INFINITY; N = BigDecimal::NAN
def t = (yield rescue "#{$!.class}")
p t { BigDecimal("17").divmod(5) }, t { BigDecimal("-17").divmod(5) }, t { BigDecimal("17.5").divmod(BigDecimal("2.5")) }
p t { BigDecimal("1").divmod(N) }, t { N.divmod(1) }, t { BigDecimal("3").divmod(I) }, t { BigDecimal("-3").divmod(I) }, t { BigDecimal("3").divmod(-I) }, t { I.divmod(3) }, t { I.divmod(I) }
p t { BigDecimal("0").divmod(3) }, t { BigDecimal("3").divmod(0) }, t { BigDecimal("0").divmod(I) }
p t { BigDecimal("1") % I }, t { BigDecimal("1") % -I }, t { BigDecimal("-1") % I }, t { I % 3 }, t { BigDecimal("1") % N }, t { BigDecimal(1) % 0 }
p t { BigDecimal("1").remainder(0) }, t { BigDecimal("0").remainder(0) }, t { BigDecimal("5").remainder(I) }, t { I.remainder(3) }, t { BigDecimal("5").remainder(N) }, t { BigDecimal("-5").remainder(3) }
p t { BigDecimal("17").div(5) }, t { BigDecimal("17").div(I) }, t { I.div(3) }, t { BigDecimal("17").div(N) }
p t { BigDecimal("17").modulo(I) }, t { BigDecimal("0").remainder(I) }
p 0.1.to_d, (0.1+0.2).to_d, BigDecimal(1.2345678901234567, 0), BigDecimal(123456789.12345678), 1.234.to_d(2), BigDecimal(0.1), BigDecimal::VERSION >= "4"
