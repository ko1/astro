# Numeric — generic #coerce for custom Numeric subclasses.  Integer/Float/Rational
# have their own (C) #coerce; this is the fallback now that Numeric is a real class.
class Numeric
  def coerce(other)
    return [other, self] if other.instance_of?(self.class)
    [Float(other), Float(self)]
  end
end

# Rational sign predicates: a reduced Rational keeps den > 0, so the sign is the
# numerator's (Integer#< handles Fixnum and Bignum).  Complex has no </> (not
# orderable), so these stay Rational-specific rather than living on Numeric.
class Rational
  def negative?; numerator < 0; end
  def positive?; numerator > 0; end
end

# Complex: predicates/conversions valid only when the imaginary part is zero.
class Complex
  def zero?; real.zero? && imaginary.zero?; end
  def to_i; imaginary.zero? ? real.to_i : raise(RangeError, "can't convert #{self} into Integer"); end
  def to_f; imaginary.zero? ? real.to_f : raise(RangeError, "can't convert #{self} into Float"); end
  def to_r; imaginary.zero? ? real.to_r : raise(RangeError, "can't convert #{self} into Rational"); end
end
