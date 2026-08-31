# Numeric — generic #coerce for custom Numeric subclasses.  Integer/Float/Rational
# have their own (C) #coerce; this is the fallback now that Numeric is a real class.
class Numeric
  def coerce(other)
    return [other, self] if other.instance_of?(self.class)
    [Float(other), Float(self)]
  end
  # Generic fallbacks for Numeric subclasses (Integer/Float/Rational/Complex have
  # their own C implementations, which take precedence).
  def abs; self < 0 ? -self : self; end
  alias magnitude abs                     # CRuby: #magnitude is an alias of #abs (same UnboundMethod)
  def -@; a, b = coerce(0); a - b; end    # CRuby Numeric#-@: 0 - self via #coerce (subtract 2nd from 1st)
  def abs2; self * self; end
  def arg; self < 0 ? Math::PI : 0; end   # angle: 0 for non-negative reals, PI for negative (Float/Integer have C impls)
  alias angle arg
  alias phase arg
  def rectangular; [self, 0]; end         # a real's rectangular form (Complex overrides)
  alias rect rectangular
  def polar; [abs, arg]; end
  def conjugate; self; end
  alias conj conjugate
  def zero?; self == 0; end
  def nonzero?; zero? ? nil : self; end
  def negative?; self < 0; end
  def positive?; self > 0; end
  def integer?; false; end
  def numerator; to_r.numerator; end       # CRuby: delegates to the Rational form
  def denominator; to_r.denominator; end
  def to_int; to_i; end
  def div(other)
    raise ZeroDivisionError, "divided by 0" if 0 == other   # CRuby num_div checks before dispatching #/
    (self / other).floor
  end
  def %(other); self - other * self.div(other); end
  alias modulo %                          # CRuby: #modulo is an alias of #%
  # CRuby num_divmod is [num_div, num_modulo]; #/ is therefore dispatched twice.
  def divmod(other); [self.div(other), self % other]; end
  def dup; self; end                      # a Numeric is its own copy
  def clone(freeze: nil)
    raise ArgumentError, "can't unfreeze #{self.class}" if freeze == false
    self
  end
  # CRuby num_eql: values of different types are never eql?; same type defers to #==
  def eql?(other)
    return false unless other.is_a?(Numeric) && self.class == other.class
    (self == other) ? true : false
  end
  # CRuby's num_remainder: a non-Numeric argument is coerced first, then the
  # sign test is two explicit comparisons per side, in this order (a Numeric
  # subclass may define only the operators CRuby actually uses).
  def remainder(other)
    a, b = self, other
    a, b = other.coerce(self) unless other.is_a?(Numeric)
    z = a % b
    if z != 0 && ((a < 0 && b > 0) || (a > 0 && b < 0))
      return a if b.is_a?(Float) && b.infinite?
      z - b
    else
      z
    end
  end
  def fdiv(other); self.to_f.fdiv(other); end
  def quo(other); self.to_r.quo(other); end
  # Rounding fallbacks: CRuby's Numeric converts to Float and delegates.
  def floor(ndigits = 0); to_f.floor(ndigits); end
  def ceil(ndigits = 0); to_f.ceil(ndigits); end
  def round(ndigits = 0); to_f.round(ndigits); end
  def truncate(ndigits = 0); to_f.truncate(ndigits); end
  # Complex-number protocol for a real Numeric (Complex overrides these).
  def real; self; end
  def imaginary; 0; end
  alias imag imaginary
  def finite?; true; end        # Float's C impl overrides for NaN/Infinity
  def infinite?; nil; end       # Float's C impl overrides
  def +@; self; end             # unary plus is identity for every Numeric
  # Numeric#step fallback (Integer/Float have C impls that take precedence).
  # Supports positional (limit, step) and keyword (by:, to:) forms; without a
  # block returns an Enumerator.
  # `1.step(5, "foo")` with no block defers the "step requires numeric arguments"
  # error to iteration — and to #size, which CRuby also raises from.
  private def __step_bad_enum(limit, st)
    e = to_enum(:step, limit, st)
    def e.size = raise(ArgumentError, "step requires numeric arguments")
    e
  end

  def step(limit = nil, step_by = nil, by: nil, to: nil, &block)
    limit = to unless to.nil?
    st = !by.nil? ? by : (step_by.nil? ? 1 : step_by)
    return to_enum(:step, limit, st) unless block_given?
    raise ArgumentError, "step can't be 0" if st == 0
    i = self
    if st > 0
      while limit.nil? || i <= limit; yield i; i += st; end
    else
      while limit.nil? || i >= limit; yield i; i += st; end
    end
    self
  end
end

# Rational sign predicates: a reduced Rational keeps den > 0, so the sign is the
# numerator's (Integer#< handles Fixnum and Bignum).  Complex has no </> (not
# orderable), so these stay Rational-specific rather than living on Numeric.
class Rational
  def negative?; numerator < 0; end
  def positive?; numerator > 0; end
  def real?; true; end
  # coerce keeps exactness: Integer/Rational -> Rational, Float -> Float.
  # A Complex with an exact zero imaginary part is exact too; anything else is
  # not coercible (CRuby raises rather than going through Float()).
  def coerce(other)
    case other
    when Integer  then [Rational(other, 1), self]
    when Rational then [other, self]
    when Float    then [other, to_f]
    when Complex
      if other.imaginary.zero? && !other.imaginary.is_a?(Float)
        [Rational(other.real, 1), self]
      else
        [other, Complex(self, 0)]
      end
    else raise TypeError, "#{other.class} can't be coerced into #{self.class}"
    end
  end
end

# Complex: predicates/conversions valid only when the imaginary part is zero.
class Complex
  def zero?; real.zero? && imaginary.zero?; end
  # to_i/to_f require an *exact* zero imaginary part: a Float 0.0 raises (unlike to_r).
  def to_i; (imaginary.zero? && !imaginary.is_a?(Float)) ? real.to_i : raise(RangeError, "can't convert #{self} into Integer"); end
  def to_f; (imaginary.zero? && !imaginary.is_a?(Float)) ? real.to_f : raise(RangeError, "can't convert #{self} into Float"); end
  def to_r; imaginary.zero? ? real.to_r : raise(RangeError, "can't convert #{self} into Rational"); end
  def finite?; real.finite? && imaginary.finite?; end
  def infinite?; (real.infinite? || imaginary.infinite?) ? 1 : nil; end
  def real?; false; end
  def +@; self; end
  def -@; Complex(-real, -imaginary); end
  def coerce(other)
    case other
    when Complex then [other, self]
    when Numeric then [Complex(other, 0), self]
    else raise TypeError, "#{other.class} can't be coerced into Complex"
    end
  end
  # CRuby undefines the ordering/rounding half of Numeric on Complex.
  undef_method :%, :<, :<=, :>, :>=, :between?, :clamp, :div, :divmod,
               :floor, :ceil, :modulo, :round, :step, :truncate,
               :negative?, :positive?, :remainder
end

class Range
  # A #succ-driven generator for an endless non-Integer range ("a".. etc.);
  # Range#each (C) returns this when it cannot materialize.
  def __each_endless_enum
    r = self
    Enumerator.new do |y|
      cur = r.begin
      loop { y << cur; cur = cur.succ }
    end
  end
end
