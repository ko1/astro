# Bignum <-> Float comparisons are exact (no lossy cast), and ~Bignum uses
# two's-complement over GMP. vs ruby.
b = 2**64 + 39
p(b <= (b + 0.0))
p(b < (b + 0.0))
p(b > (b + 0.0))
p(b == (b + 0.0))
p((b + 0.0) < b)
p((b + 0.0) <=> b)
p(b <=> b.to_f)
p(b <= (b + 9999.0))
p(2**64 == 1.8446744073709552e+19)
p((2**60 + 1) == (2**60 + 1).to_f)
p(~5)
p(~(2**70))
p(~18446744073709551615)
p(~(-(2**100)))
p(5 == 5.0)
p(3.5 == 3)
