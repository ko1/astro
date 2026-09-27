# BigDecimal op Rational (either side) converts the Rational at CRuby's coerce
# precision (the receiver's 9-digit words, at least 32 digits); -W:category
# options set Warning[category].
require 'rbconfig'
require 'bigdecimal'
a = BigDecimal("1.5"); r = Rational(1, 3)
p a * r, r * a, a + r, r + a, a - r, r - a, a / r, r / a
b = BigDecimal("123456789012345678901234567890.123456789")
p b * r, r * b, b + r
c = BigDecimal("0.000000000001")
p c * r, r * c, c.coerce(r), a.coerce(r)
p BigDecimal("1") * Rational(2, 7), Rational(2, 7) <=> BigDecimal("0.3"), BigDecimal("0.3") == Rational(3, 10)
["-W:performance", "-w -W:no-deprecated", "-W:no-experimental", "-W:strict_unused_block", "-W:bogus"].each do |f|
  out = IO.popen([RbConfig.ruby, *f.split, "-e", "p [Warning[:performance], Warning[:deprecated], Warning[:experimental]]; warn 'pw', category: :performance"], err: [:child, :out], &:read)
  p [f, out.sub(%r{\A\S+: warning}, "ruby: warning")]
end
