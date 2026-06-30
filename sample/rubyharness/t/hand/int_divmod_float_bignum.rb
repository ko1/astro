# Integer#divmod(Float): bignum-range quotient + NaN → FloatDomainError. vs ruby.
p (2**62).divmod(1.0)
p (2**70).divmod(2.0)
p 10.divmod(3.0)
p (-10).divmod(3.0)
p 7.divmod(2.0)
p (2**100).divmod(3.0)
begin; 5.divmod(Float::NAN); rescue => e; p e.class; end
begin; (2**70).divmod(Float::NAN); rescue => e; p e.class; end
begin; 5.divmod(0.0); rescue => e; p e.class; end
