# Integer#polar returns [self.abs, angle] with an EXACT magnitude (a Bignum
# stayed exact, not cast to Float). vs ruby.
p (10**86).polar
p (-(10**86)).polar
p 5.polar
p (-5).polar
p 0.polar
p (2**100).polar[0]
p (2**100).polar[0] == 2**100
