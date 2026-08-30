# bigdecimal — arbitrary-precision decimal arithmetic in pure Ruby.
#
# A finite value is (sign, digits, exponent): the number is
# `sign * 0.digits * 10**exponent`, i.e. `digits` is a positive Integer read as
# a fraction with the point in front of it.  That is exactly the form CRuby's
# BigDecimal#to_s prints ("0.15e1"), so the string round-trip is direct.
# Non-finite values carry digits == 0 and a `special` tag.
class BigDecimal < Numeric
  ROUND_UP          = 1
  ROUND_DOWN        = 2
  ROUND_HALF_UP     = 3
  ROUND_HALF_DOWN   = 4
  ROUND_CEILING     = 5
  ROUND_FLOOR       = 6
  ROUND_HALF_EVEN   = 7
  ROUND_MODE        = 256
  SIGN_NaN          = 0
  SIGN_POSITIVE_ZERO = 1
  SIGN_NEGATIVE_ZERO = -1
  SIGN_POSITIVE_FINITE = 2
  SIGN_NEGATIVE_FINITE = -2
  SIGN_POSITIVE_INFINITE = 3
  SIGN_NEGATIVE_INFINITE = -3
  EXCEPTION_INFINITY  = 1
  EXCEPTION_NaN       = 2
  EXCEPTION_UNDERFLOW = 4
  EXCEPTION_OVERFLOW  = 1
  EXCEPTION_ZERODIVIDE = 16
  EXCEPTION_ALL       = 255
  BASE                = 10000
  VERSION             = "3.2.4"

  # 20 significant digits is the default working precision for division and
  # other inexact operations (CRuby grows it from the operands' precision).
  DEFAULT_PREC = 20
  private_constant :DEFAULT_PREC

  # CRuby keeps the decimal exponent in a machine word; past that a value
  # overflows to Infinity (or underflows to zero).
  EXP_LIMIT__ = 2**63
  private_constant :EXP_LIMIT__

  @@limit      = 0
  @@round_mode = ROUND_HALF_UP
  @@exceptions = 0

  class << self
    def limit(n = nil)
      old = @@limit
      unless n.nil?
        raise TypeError, "wrong argument type #{n.class} (expected Integer)" unless n.is_a?(Integer)
        raise ArgumentError, "argument must be positive" if n < 0
        @@limit = n
      end
      old
    end

    # ROUND_MODE gets/sets the rounding mode; every other flag toggles a bit in
    # the exception mask and the whole mask is returned.
    def mode(mode, value = nil)
      raise TypeError, "wrong argument type #{mode.class} (expected Integer)" unless mode.is_a?(Integer)
      if mode == ROUND_MODE
        return @@round_mode if value.nil?
        __round_kind(value)                              # validates
        @@round_mode = value
        return value
      end
      unless value.nil?
        unless value == true || value == false
          raise TypeError, "second argument must be true or false"
        end
        @@exceptions = value ? (@@exceptions | mode) : (@@exceptions & ~mode)
      end
      @@exceptions
    end

    def __round_mode = @@round_mode

    def __round_kind(mode)
      case mode
      when ROUND_UP, :up then :ceil_abs
      when ROUND_DOWN, :down, :truncate then :truncate
      when ROUND_CEILING, :ceiling, :ceil then :ceil
      when ROUND_FLOOR, :floor then :floor
      when ROUND_HALF_UP, :half_up, :default then :half_up
      when ROUND_HALF_DOWN, :half_down then :half_down
      when ROUND_HALF_EVEN, :banker, :half_even then :half_even
      else raise ArgumentError, "invalid rounding mode (#{mode})"
      end
    end

    def double_fig = 16

    def _load(str) = BigDecimal(str)

    def interpret_loosely(str) = __parse(str, false)

    # The internal constructor: `sign` is +1/-1, `digits` a non-negative
    # Integer with no trailing zeros, `exp` the decimal exponent.
    def __new(sign, digits, exp, special = nil)
      if special.nil? && !digits.zero?
        if exp >= EXP_LIMIT__                            # overflow
          special = :inf
          digits = 0
        elsif exp <= -EXP_LIMIT__                        # underflow to zero
          digits = 0
          exp = 0
        end
      end
      if special == :nan && (@@exceptions & EXCEPTION_NaN) != 0
        raise FloatDomainError, "Computation results in 'NaN'(Not a Number)"
      end
      if special == :inf && (@@exceptions & EXCEPTION_INFINITY) != 0
        raise FloatDomainError, "Computation results to 'Infinity'"
      end
      v = allocate
      v.send(:__setup, sign, digits, exp, special)
      v
    end

    def __parse(str, strict)
      s = str.to_s.strip.gsub(/(?<=\d)_(?=\d)/, '')     # 12_345.67 like Float()
      m = /\A([+-]?)(Infinity|inf)\z/i.match(s) and
        return __new(m[1] == '-' ? -1 : 1, 0, 0, :inf)
      return __new(1, 0, 0, :nan) if /\ANaN\z/i.match?(s)
      m = /\A([+-]?)(\d*)(?:\.(\d*))?(?:[eEdD]([+-]?\d+))?/.match(s)
      unless m && (!m[2].empty? || (m[3] && !m[3].empty?))
        raise ArgumentError, "invalid value for BigDecimal(): #{str.inspect}" if strict
        return __new(1, 0, 0)
      end
      if strict && m.end(0) != s.length
        raise ArgumentError, "invalid value for BigDecimal(): #{str.inspect}"
      end
      int, frac = m[2].to_s, m[3].to_s
      exp = (m[4] || "0").to_i + int.length
      digits = (int + frac)
      # strip leading zeros (each one lowers the exponent)
      lead = digits.length - digits.sub(/\A0+/, '').length
      digits = digits[lead..] || ""
      exp -= lead
      __new(m[1] == '-' ? -1 : 1, digits.to_i, exp)
    end
  end

  private def __setup(sign, digits, exp, special)
    @sign = sign
    @special = special
    if special
      @digits = 0
      @exp = 0
    else
      # canonical form: no trailing zeros in `digits`
      if digits.zero?
        @digits = 0
        @exp = 0
      else
        while (digits % 10).zero?
          digits /= 10
        end
        @digits = digits
        @exp = exp
      end
    end
    freeze
  end
  # called from the class-level constructor, so it cannot be protected

  def __digits = @digits
  def __digits_pub = @digits
  def __exp = @exp
  def __special = @special
  protected :__digits, :__exp, :__special

  def nan? = @special == :nan
  def infinite? = @special == :inf ? @sign : nil
  def finite? = @special.nil?
  def zero? = finite? && @digits.zero?
  def nonzero? = zero? ? nil : self
  def positive? = !nan? && @sign > 0 && !zero?
  def negative? = !nan? && @sign < 0 && !zero?

  def sign
    return SIGN_NaN if nan?
    return @sign > 0 ? SIGN_POSITIVE_INFINITE : SIGN_NEGATIVE_INFINITE if @special == :inf
    return @sign > 0 ? SIGN_POSITIVE_ZERO : SIGN_NEGATIVE_ZERO if zero?
    @sign > 0 ? SIGN_POSITIVE_FINITE : SIGN_NEGATIVE_FINITE
  end

  def exponent = finite? ? @exp : 0
  def precision = finite? ? __ndigits : 0
  def scale = finite? ? [__ndigits - @exp, 0].max : 0
  def precs = [precision, precision]
  def n_significant_digits = zero? ? 0 : __ndigits
  def __ndigits = @digits.zero? ? 0 : @digits.to_s.length
  protected :__ndigits

  def to_s(fmt = nil) = __to_s(fmt)

  # #inspect must not go through #to_s: a prepended override of #to_s changes
  # only what #to_s prints (CRuby).
  def inspect = __to_s(nil)

  private def __to_s(fmt)
    return (@sign < 0 ? "-Infinity" : "Infinity").force_encoding(Encoding::US_ASCII) if @special == :inf
    return "NaN".dup.force_encoding(Encoding::US_ASCII) if nan?
    fmt = fmt.to_s
    group = fmt[/\d+/].to_i                              # "5" / "5F" → a space every 5 digits
    sign = @sign < 0 ? "-" : (fmt.include?("+") ? "+" : (fmt.include?(" ") ? " " : ""))
    return "#{sign}0.0".force_encoding(Encoding::US_ASCII) if @digits.zero?
    ds = @digits.to_s
    if group > 0
      body = if fmt.downcase.include?("f")
               if @exp <= 0            then ["0", "0" * (-@exp) + ds]
               elsif @exp >= ds.length then [ds + "0" * (@exp - ds.length), "0"]
               else                         [ds[0, @exp], ds[@exp..]]
               end
             else
               ["0", ds]
             end
      lead = body[0].reverse.scan(/.{1,#{group}}/).map(&:reverse).reverse.join(" ")
      tail = body[1].scan(/.{1,#{group}}/).join(" ")
      return ("#{sign}#{lead}.#{tail}" + (fmt.downcase.include?("f") ? "" : "e#{@exp}")).force_encoding(Encoding::US_ASCII)
    end
    body = if fmt.downcase.include?("f")
             if @exp <= 0            then "#{sign}0." + "0" * (-@exp) + ds
             elsif @exp >= ds.length then "#{sign}#{ds}#{'0' * (@exp - ds.length)}.0"
             else                         "#{sign}#{ds[0, @exp]}.#{ds[@exp..]}"
             end
           else
             "#{sign}0.#{ds}e#{@exp}"
           end
    body.force_encoding(Encoding::US_ASCII)               # CRuby: always US-ASCII
  end

  def to_json(*) = to_s.dump

  def to_i
    raise FloatDomainError, to_s unless finite?
    return 0 if @exp <= 0 || @digits.zero?
    ds = @digits.to_s
    v = @exp >= ds.length ? @digits * 10**(@exp - ds.length) : ds[0, @exp].to_i
    @sign * v
  end
  alias to_int to_i

  def to_f
    return Float::NAN if nan?
    return @sign > 0 ? Float::INFINITY : -Float::INFINITY if @special == :inf
    to_s.to_f
  end

  def to_r
    raise FloatDomainError, to_s unless finite?
    ds = @digits.to_s
    Rational(@sign * @digits, 1) * Rational(10) ** (@exp - ds.length)
  end

  def to_d = self
  def coerce(other)
    case other
    when BigDecimal then [other, self]
    when Integer, Rational then [BigDecimal(other, 0), self]
    when Float then [BigDecimal(other.to_s), self]
    else raise TypeError, "#{other.class} can't be coerced into BigDecimal"
    end
  end

  def hash = [@sign, @digits, @exp, @special].hash
  def dup = self
  alias clone dup                                        # CRuby: the same method
  def frozen? = true

  # --- arithmetic -----------------------------------------------------------

  # Both operands as (sign*integer, shift) with a common exponent.
  private def __aligned(other)
    e = [@exp - __ndigits, other.__exp - other.__ndigits].min
    [@sign * @digits * 10**((@exp - __ndigits) - e),
     other.__sign * other.__digits * 10**((other.__exp - other.__ndigits) - e), e]
  end

  def __sign = @sign
  protected :__sign

  private def __coerce_operand(other)
    case other
    when BigDecimal then other
    when Integer    then BigDecimal(other, 0)
    when Float      then BigDecimal(other.to_s)
    when Rational   then BigDecimal(other.numerator, 0).div(BigDecimal(other.denominator, 0), DEFAULT_PREC)
    else nil
    end
  end

  private def __from_scaled(iv, e)
    return BigDecimal.__new(1, 0, 0) if iv.zero?
    sign = iv < 0 ? -1 : 1
    iv = iv.abs
    v = BigDecimal.__new(sign, iv, e + iv.to_s.length)
    @@limit.zero? ? v : v.__round_sig(@@limit)           # BigDecimal.limit caps every result
  end

  def +(other)
    o = __coerce_operand(other) or return __coerce_fallback(other, :+)
    return __special_add(o) if !finite? || !o.finite?
    a, b, e = __aligned(o)
    __from_scaled(a + b, e)
  end

  def -(other)
    o = __coerce_operand(other) or return __coerce_fallback(other, :-)
    self + (-o)
  end

  def -@ = @special ? BigDecimal.__new(-@sign, 0, 0, @special) : BigDecimal.__new(-@sign, @digits, @exp)
  def +@ = self
  def abs = @sign < 0 ? -self : self

  def *(other)
    o = __coerce_operand(other) or return __coerce_fallback(other, :*)
    return __special_mul(o) if !finite? || !o.finite?
    return BigDecimal.__new(@sign * o.__sign, 0, 0) if @digits.zero? || o.__digits.zero?
    e = (@exp - __ndigits) + (o.__exp - o.__ndigits)
    __from_scaled(@sign * o.__sign * @digits * o.__digits, e)
  end

  def /(other)
    o = __coerce_operand(other) or return __coerce_fallback(other, :/)
    div_impl(o, 0)
  end
  alias quo /

  def div(other, *rest)
    o = __coerce_operand(other) or return __coerce_fallback(other, :div)
    if rest.empty?                                    # no precision → integer quotient
      raise ZeroDivisionError, "divided by 0" if o.zero?
      return __unlimited { self / o }.floor            # the global limit must not truncate it
    end
    prec = rest[0]
    raise TypeError, "wrong argument type #{prec.class} (expected Integer)" unless prec.is_a?(Integer)
    raise ArgumentError, "argument must be positive" if prec < 0
    return BigDecimal.__new(1, 0, 0, :nan) if zero? && o.zero?
    div_impl(o, prec)
  end

  private def div_impl(o, prec)
    if !finite? || !o.finite?
      return BigDecimal.__new(1, 0, 0, :nan) if nan? || o.nan? || (@special == :inf && o.__special == :inf)
      return BigDecimal.__new(@sign * o.__sign, 0, 0, :inf) if @special == :inf
      return BigDecimal.__new(@sign * o.__sign, 0, 0)       # finite / Infinity
    end
    if o.zero?
      return BigDecimal.__new(1, 0, 0, :nan) if zero?     # 0/0 is NaN, not an error
      return BigDecimal.__new(@sign * o.__sign, 0, 0, :inf)
    end
    return BigDecimal.__new(@sign * o.__sign, 0, 0) if zero?
    n = prec.zero? ? [__ndigits, o.__ndigits, DEFAULT_PREC].max + DEFAULT_PREC : prec + 2
    # Scale the numerator so the quotient carries n significant digits.  With
    # both operands read as 0.digits * 10**exp, the quotient's exponent is
    #   exp_a - exp_b - n + len(q)
    # (the -shift and the nd_b - nd_a from the mantissa ratio cancel to -n).
    shift = n + o.__ndigits - __ndigits
    q = (@digits * 10**shift) / o.__digits
    e = @exp - o.__exp - n + q.to_s.length
    v = BigDecimal.__new(@sign * o.__sign, q, e)
    return v.__round_sig(prec) unless prec.zero?
    v = v.__round_sig(n - DEFAULT_PREC + 2)
    @@limit.zero? ? v : v.__round_sig(@@limit)        # BigDecimal.limit caps an unsized quotient
  end

  # Round to `n` significant digits, returning a new BigDecimal.  The rounding
  # mode is BigDecimal.mode(ROUND_MODE) unless a kind is given.
  def __round_sig(n, kind = nil)
    return self if !finite? || @digits.zero? || __ndigits <= n || n <= 0
    kind ||= BigDecimal.__round_kind(BigDecimal.__round_mode)
    pow = 10**(__ndigits - n)
    q, r = @digits.divmod(pow)
    unless r.zero?
      up = case kind
           when :truncate  then false
           when :ceil      then @sign > 0
           when :floor     then @sign < 0
           when :ceil_abs  then true
           when :half_down then r * 2 > pow
           when :half_even then (r * 2 > pow) || (r * 2 == pow && q.odd?)
           else                 r * 2 >= pow
           end
      q += 1 if up
    end
    BigDecimal.__new(@sign, q, @exp)
  end

  def %(other)
    o = __coerce_operand(other) or return __coerce_fallback(other, :%)
    raise ZeroDivisionError, "divided by 0" if o.zero?
    return BigDecimal.__new(1, 0, 0, :nan) if !finite? || !o.finite?
    a, b, e = __aligned(o)                               # exact: no rounding of the quotient
    __from_scaled(a - b * (a / b), e)                    # Integer#/ floors
  end
  alias modulo %

  def divmod(other)
    o = __coerce_operand(other) or return __coerce_fallback(other, :divmod)
    # NaN wins over the divide-by-zero check (CRuby)
    return [BigDecimal.__new(1, 0, 0, :nan), BigDecimal.__new(1, 0, 0, :nan)] if nan? || o.nan?
    raise ZeroDivisionError, "divided by 0" if o.finite? && o.zero?
    # An infinite dividend gives [±Infinity, NaN] (bigdecimal 3.x)
    if @special == :inf
      return [BigDecimal.__new(@sign * o.__sign, 0, 0, :inf), BigDecimal.__new(1, 0, 0, :nan)]
    end
    return [BigDecimal.__new(1, 0, 0, :nan), BigDecimal.__new(1, 0, 0, :nan)] unless o.finite?
    # CRuby divides first and floors the (rounded) quotient, so the pair stays
    # consistent with `self / other` even where that division loses digits.
    q = BigDecimal(__unlimited { self / o }.floor, 0)
    [q, __unlimited { self - q * o }]
  end

  def remainder(other)
    o = __coerce_operand(other) or return __coerce_fallback(other, :remainder)
    return BigDecimal.__new(1, 0, 0, :nan) if !finite? || !o.finite? || o.zero?
    a, b, e = __aligned(o)
    q = a.abs / b.abs
    q = -q if (a < 0) != (b < 0)
    __from_scaled(a - b * q, e)                          # truncating (sign of self)
  end

  def **(n)
    return BigDecimal.__new(1, 0, 0, :nan) if nan?
    raise TypeError, "wrong argument type" unless n.is_a?(Integer) || n.is_a?(Float) || n.is_a?(BigDecimal)
    n = n.to_i if n.is_a?(BigDecimal) && n == n.to_i
    unless n.is_a?(Integer)
      return BigDecimal((to_f ** n.to_f).to_s)
    end
    return BigDecimal("1") if n.zero?
    if n.negative?
      # CRuby keeps at least ndigits + DOUBLE_FIG digits; a plain #/ would cut
      # the reciprocal down to the default working precision.
      prec = [__ndigits, DEFAULT_PREC].max + 2 * BigDecimal.double_fig
      return BigDecimal("1").div(self ** (-n), prec)
    end
    r = BigDecimal("1")
    base = self
    k = n
    while k > 0
      r = r * base if k.odd?
      base = base * base
      k >>= 1
    end
    r
  end
  alias power **

  def sqrt(prec)
    raise TypeError, "wrong argument type #{prec.class} (expected Integer)" unless prec.is_a?(Integer)
    raise ArgumentError, "argument must be positive" if prec < 0
    raise FloatDomainError, "sqrt of NaN" if nan?
    raise FloatDomainError, "sqrt of negative value" if negative?
    return BigDecimal.__new(1, 0, 0, :inf) if @special == :inf
    return BigDecimal.__new(1, 0, 0) if zero?
    n = [prec.to_i, DEFAULT_PREC].max + 4
    # integer sqrt of digits scaled so the answer has >= n significant digits
    shift = 2 * n
    shift += 1 if ((@exp - __ndigits) - shift).odd?
    scaled = @digits * 10**shift
    r = Integer.sqrt(scaled)
    e2 = ((@exp - __ndigits) - shift) / 2 + r.to_s.length
    BigDecimal.__new(1, r, e2).__round_sig(prec > 0 ? prec : n - 2)
  end

  # --- rounding -------------------------------------------------------------

  def floor(n = nil) = __guard_special(n) { __round_to(n, :floor) }
  def ceil(n = nil)  = __guard_special(n) { __round_to(n, :ceil) }
  def truncate(n = nil) = __guard_special(n) { __round_to(n, :truncate) }

  # NaN / Infinity answer themselves when a precision is given, and raise
  # FloatDomainError only when asked for an Integer.
  private def __guard_special(n)
    return self if !finite? && !n.nil?
    yield
  end

  def round(*args)
    n = args[0]
    mode = args.length > 1 ? args[1] : @@round_mode
    kind = BigDecimal.__round_kind(mode)
    return self if !finite? && !n.nil?                   # round(n) leaves NaN/Infinity alone
    __round_to(n, kind)
  end

  private def __round_to(n, kind)
    raise FloatDomainError, to_s unless finite?
    digits = n.nil? ? 0 : n.to_i
    ds = @digits.to_s
    keep = @exp + digits                      # significant digits to keep
    if keep >= ds.length
      return n.nil? ? to_i : self             # nothing to drop
    end
    if keep <= 0
      dropped = @digits
      kept = 0
      half = keep == 0 ? 10**ds.length : nil
    else
      kept, dropped = @digits.divmod(10**(ds.length - keep))
    end
    pow = 10**(ds.length - (keep > 0 ? keep : 0))
    unless dropped.zero?
      up = case kind
           when :truncate  then false
           when :ceil      then @sign > 0
           when :floor     then @sign < 0
           when :ceil_abs  then true
           when :half_down then dropped * 2 > pow
           when :half_even then (dropped * 2 > pow) || (dropped * 2 == pow && kept.odd?)
           else                 dropped * 2 >= pow
           end
      kept += 1 if up
    end
    value = kept.zero? ? BigDecimal.__new(@sign, 0, 0)
                       : BigDecimal.__new(@sign, kept, @exp - (keep > 0 ? 0 : keep) + (keep > 0 ? 0 : 0)).__rescale(keep, digits)
    n.nil? ? value.to_i : value
  end

  # Rebuild with `keep` significant digits at decimal position `digits`.
  def __rescale(keep, digits)
    return self if @digits.zero?
    BigDecimal.__new(@sign, @digits, keep > 0 ? @exp : -digits + __ndigits)
  end
  protected :__rescale

  def fix = finite? ? BigDecimal(truncate(0).to_s) : self
  def frac = finite? ? self - fix : self

  def add(other, prec)
    raise TypeError, "wrong argument type #{prec.class} (expected Integer)" unless prec.is_a?(Integer)
    raise ArgumentError, "argument must be positive" if prec < 0
    r = __unlimited { self + other }
    prec.zero? ? (@@limit.zero? ? r : r.__round_sig(@@limit)) : r.__round_sig(prec)
  end
  def sub(other, prec)
    raise TypeError, "wrong argument type #{prec.class} (expected Integer)" unless prec.is_a?(Integer)
    raise ArgumentError, "argument must be positive" if prec < 0
    r = __unlimited { self - other }
    prec.zero? ? (@@limit.zero? ? r : r.__round_sig(@@limit)) : r.__round_sig(prec)
  end
  def mult(other, prec)
    raise TypeError, "wrong argument type #{prec.class} (expected Integer)" unless prec.is_a?(Integer)
    raise ArgumentError, "argument must be positive" if prec < 0
    r = __unlimited { self * other }
    prec.zero? ? (@@limit.zero? ? r : r.__round_sig(@@limit)) : r.__round_sig(prec)
  end

  # Run a block with BigDecimal.limit suspended (an explicit precision argument
  # takes priority over the global limit).
  private def __unlimited
    saved = @@limit
    @@limit = 0
    begin
      yield
    ensure
      @@limit = saved
    end
  end

  # --- comparison -----------------------------------------------------------

  def <=>(other)
    o = __coerce_operand(other)
    if o.nil? && other.respond_to?(:coerce)     # any Numeric-ish via #coerce
      a, b = other.coerce(self)
      return a <=> b
    end
    return nil if o.nil?
    return nil if nan? || o.nan?
    if @special == :inf || o.__special == :inf
      a = @special == :inf ? @sign * 2 : (zero? ? 0 : @sign)
      b = o.__special == :inf ? o.__sign * 2 : (o.zero? ? 0 : o.__sign)
      return a <=> b if a != b || @special == :inf || o.__special == :inf
    end
    a, b, = __aligned(o)
    a <=> b
  end

  def ==(other)
    return false if nan?
    c = (self <=> other)
    !c.nil? && c.zero?
  end
  alias === ==
  alias eql? ==                                          # CRuby: the same method

  def <(other)  = __cmp_op(other, :<)
  def <=(other) = __cmp_op(other, :<=)
  def >(other)  = __cmp_op(other, :>)
  def >=(other) = __cmp_op(other, :>=)

  private def __cmp_op(other, op)
    return false if nan?                                 # any comparison with NaN is false
    o = __coerce_operand(other)
    return false if o&.nan?
    c = (self <=> other)
    raise ArgumentError, "comparison of BigDecimal with #{other.inspect} failed" if c.nil?
    c.send(op, 0)
  end

  private def __coerce_fallback(other, op)
    if other.respond_to?(:coerce)
      a, b = other.coerce(self)
      return a.send(op, b)
    end
    raise TypeError, "#{other.class} can't be coerced into BigDecimal"
  end

  private def __special_add(o)
    return BigDecimal.__new(1, 0, 0, :nan) if nan? || o.nan?
    if @special == :inf && o.__special == :inf
      return @sign == o.__sign ? self : BigDecimal.__new(1, 0, 0, :nan)
    end
    @special == :inf ? self : o
  end

  private def __special_mul(o)
    return BigDecimal.__new(1, 0, 0, :nan) if nan? || o.nan?
    return BigDecimal.__new(1, 0, 0, :nan) if (zero? && o.__special == :inf) || (o.zero? && @special == :inf)
    BigDecimal.__new(@sign * o.__sign, 0, 0, :inf)
  end

  def _dump(_limit = nil) = "#{precision}:#{to_s}"
  def split
    return [0, "NaN", 10, 0] if nan?
    return [@sign, "Infinity", 10, 0] if @special == :inf
    [sign <=> 0, @digits.to_s, 10, @exp]
  end

  NAN      = __new(1, 0, 0, :nan)
  INFINITY = __new(1, 0, 0, :inf)
end

# Kernel#BigDecimal(value, exception: true) — the public constructor.
module Kernel
  def BigDecimal(value, precision = 0, exception: true)
    case value
    when BigDecimal then value
    when Integer
      return BigDecimal.__new(1, 0, 0) if value.zero?
      s = value.abs.to_s
      BigDecimal.__new(value.negative? ? -1 : 1, s.to_i, s.length)
    when Float
      return BigDecimal.__new(1, 0, 0, :nan) if value.nan?
      return BigDecimal.__new(value > 0 ? 1 : -1, 0, 0, :inf) if value.infinite?
      unless precision.is_a?(Integer) && (precision > 0 || value == value.to_i)
        raise ArgumentError, "can't omit precision for a Float" if precision.zero? && value != value.to_i
      end
      v = BigDecimal.__parse(value.to_s, false)
      precision.to_i > 0 ? v.__round_sig(precision.to_i) : v
    when Rational
      raise ArgumentError, "can't omit precision for a Rational" if precision.to_i <= 0
      BigDecimal(value.numerator, 0).div(BigDecimal(value.denominator, 0), precision.to_i)
    when nil
      raise TypeError, "can't convert nil into BigDecimal" if exception
      nil
    when String
      begin
        v = BigDecimal.__parse(value, true)
      rescue ArgumentError
        raise if exception
        return nil
      end
      precision.to_i > 0 ? v.__round_sig(precision.to_i) : v
    else
      if value.respond_to?(:to_str)
        BigDecimal(value.to_str, precision, exception: exception)
      elsif exception
        raise TypeError, "can't convert #{value.class} into BigDecimal"
      end
    end
  end
  module_function :BigDecimal
  public :BigDecimal
end

class Integer
  def to_d = BigDecimal(self, 0)
end

class Float
  def to_d(prec = 0) = BigDecimal(self.to_s, prec)
end

class String
  # #to_d is the LOOSE parse: it takes the leading numeric part and yields 0 for
  # a string that does not start with one (unlike Kernel#BigDecimal).
  def to_d = BigDecimal.__parse(self, false)
end

class NilClass
  def to_d = BigDecimal("0")
end

class Rational
  def to_d(prec) = BigDecimal(self, prec)
end

# BigMath — the transcendental functions, computed by series to `prec`
# significant digits.  Enough for the documented API; not tuned for speed.
module BigMath
  module_function

  def exp(x, prec)
    raise ArgumentError, "Zero or negative precision for exp" if prec.to_i <= 0
    x = BigDecimal(x, prec) unless x.is_a?(BigDecimal)
    return BigDecimal("NaN") if x.nan?
    return x.infinite? == 1 ? BigDecimal("Infinity") : BigDecimal("0") if x.infinite?
    n = prec + 10
    term = BigDecimal("1")
    sum = BigDecimal("1")
    (1..(n * 4)).each do |k|
      term = (term * x).div(BigDecimal(k, 0), n)
      break if term.zero?
      sum = sum + term
      break if term.abs < BigDecimal("1e-#{n}")
    end
    sum.__round_sig(prec)
  end

  def log(x, prec)
    raise ArgumentError, "Zero or negative precision for log" if prec.to_i <= 0
    x = BigDecimal(x, prec) unless x.is_a?(BigDecimal)
    raise Math::DomainError, "Zero or negative argument for log" if x.negative? || x.zero?
    return BigDecimal("Infinity") if x.infinite? == 1
    return BigDecimal("NaN") if x.nan?
    n = prec + 10
    # atanh series: log(x) = 2*atanh((x-1)/(x+1)), fastest near 1, so scale by
    # powers of ten first.
    shift = x.exponent
    x = BigDecimal.__new(1, x.__digits_pub, 0)
    z = (x - 1).div(x + 1, n)
    z2 = z.mult(z, n)
    term = z
    sum = z
    k = 3
    while !term.zero? && term.abs > BigDecimal("1e-#{n}")
      term = term.mult(z2, n)
      sum = sum + term.div(BigDecimal(k, 0), n)
      k += 2
    end
    ln10 = LOG10__(n)
    (sum * 2 + ln10 * shift).__round_sig(prec)
  end

  def LOG10__(n)
    # log(10) = 2*atanh(9/11)
    z = BigDecimal(9, 0).div(BigDecimal(11, 0), n)
    z2 = z.mult(z, n)
    term = z
    sum = z
    k = 3
    while term.abs > BigDecimal("1e-#{n}")
      term = term.mult(z2, n)
      sum = sum + term.div(BigDecimal(k, 0), n)
      k += 2
    end
    sum * 2
  end

  def PI(prec)
    raise ArgumentError, "Zero or negative precision for PI" if prec.to_i <= 0
    n = prec + 10
    # Machin: pi/4 = 4*atan(1/5) - atan(1/239)
    (__atan_inv(5, n) * 4 - __atan_inv(239, n)).mult(BigDecimal(4, 0), n).__round_sig(prec)
  end

  def E(prec)
    raise ArgumentError, "Zero or negative precision for E" if prec.to_i <= 0
    exp(BigDecimal("1"), prec)
  end

  def sqrt(x, prec) = BigDecimal(x, prec).sqrt(prec)

  def sin(x, prec)
    raise ArgumentError, "Zero or negative precision for sin" if prec.to_i <= 0
    x = BigDecimal(x, prec) unless x.is_a?(BigDecimal)
    return BigDecimal("NaN") if x.nan? || x.infinite?
    __series(x, prec, true)
  end

  def cos(x, prec)
    raise ArgumentError, "Zero or negative precision for cos" if prec.to_i <= 0
    x = BigDecimal(x, prec) unless x.is_a?(BigDecimal)
    return BigDecimal("NaN") if x.nan? || x.infinite?
    __series(x, prec, false)
  end

  def atan(x, prec)
    raise ArgumentError, "Zero or negative precision for atan" if prec.to_i <= 0
    x = BigDecimal(x, prec) unless x.is_a?(BigDecimal)
    return BigDecimal("NaN") if x.nan?
    n = prec + 10
    if x.abs > BigDecimal("1")
      s = x.negative? ? -1 : 1
      return (PI(n).div(BigDecimal(2, 0), n) * s - atan(BigDecimal("1").div(x, n), n)).__round_sig(prec)
    end
    # Halve the argument until the series converges quickly (at |x| == 1 the
    # plain series does not converge at all):
    #   atan(x) = 2 * atan(x / (1 + sqrt(1 + x^2)))
    halvings = 0
    while x.abs > BigDecimal("0.5")
      x = x.div(BigDecimal("1") + (BigDecimal("1") + x.mult(x, n)).sqrt(n), n)
      halvings += 1
    end
    term = x
    sum = x
    x2 = x.mult(x, n)
    k = 3
    while !term.zero? && term.abs > BigDecimal("1e-#{n}")
      term = term.mult(x2, n)
      sum = sum + (term.div(BigDecimal(k, 0), n)) * (k % 4 == 3 ? -1 : 1)
      k += 2
    end
    halvings.times { sum = sum * 2 }
    sum.__round_sig(prec)
  end

  def __atan_inv(m, n)
    z = BigDecimal(1, 0).div(BigDecimal(m, 0), n)
    z2 = z.mult(z, n)
    term = z
    sum = z
    k = 3
    while term.abs > BigDecimal("1e-#{n}")
      term = term.mult(z2, n)
      sum = sum + term.div(BigDecimal(k, 0), n) * (k % 4 == 3 ? -1 : 1)
      k += 2
    end
    sum
  end

  def __series(x, prec, odd)
    n = prec + 10
    term = odd ? x : BigDecimal("1")
    sum = term
    x2 = x.mult(x, n)
    k = odd ? 2 : 1
    sign = -1
    while !term.zero? && term.abs > BigDecimal("1e-#{n}")
      term = term.mult(x2, n).div(BigDecimal(k * (k + 1), 0), n)
      sum = sum + term * sign
      sign = -sign
      k += 2
    end
    sum.__round_sig(prec)
  end
end
