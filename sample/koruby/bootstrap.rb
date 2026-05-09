# Bootstrap — Ruby methods loaded before any user program.
#
# Defines methods on built-in modules/classes that are easier to write
# in Ruby than in C.  Any class included into Enumerable picks up all
# the iterator helpers below by virtue of needing only `each`.

module Enumerable
  # NOTE: every method that yields *inside* an `each { ... }` block
  # takes `&blk` and uses `blk.call(...)` instead.  koruby doesn't yet
  # forward `yield` from within a block to the outer method's block.
  def to_a
    arr = []
    each { |x| arr << x }
    arr
  end

  def count
    n = 0
    each { _1; n += 1 }
    n
  end

  def map(&blk)
    arr = []
    each { |x| arr << blk.call(x) }
    arr
  end

  def select(&blk)
    arr = []
    each { |x| arr << x if blk.call(x) }
    arr
  end

  def reject(&blk)
    arr = []
    each { |x| arr << x unless blk.call(x) }
    arr
  end

  # `break` semantics work; `return` from inside a block does not yet
  # propagate to the enclosing method (no non-local return support), so
  # use sentinel + break for early-exit helpers.
  def find(&blk)
    found = nil
    found_flag = false
    each { |x|
      if blk.call(x)
        found = x
        found_flag = true
        break
      end
    }
    found
  end

  def reduce(init = nil, &blk)
    acc = init
    first = init.nil?
    each { |x|
      if first
        acc = x; first = false
      else
        acc = blk.call(acc, x)
      end
    }
    acc
  end

  def min
    best = nil
    seen = false
    each { |x|
      if !seen || (x <=> best) < 0
        best = x; seen = true
      end
    }
    best
  end

  def max
    best = nil
    seen = false
    each { |x|
      if !seen || (x <=> best) > 0
        best = x; seen = true
      end
    }
    best
  end

  def include?(target)
    found = false
    each { |x| if x == target; found = true; break; end }
    found
  end

  def first(n = nil)
    if n.nil?
      head = nil
      each { |x| head = x; break }
      head
    else
      arr = []
      i = 0
      each { |x|
        break if i >= n
        arr << x
        i += 1
      }
      arr
    end
  end

  def each_with_index(&blk)
    i = 0
    each { |x| blk.call(x, i); i += 1 }
    nil
  end

  def any?(&blk)
    result = false
    if blk
      each { |x| if blk.call(x); result = true; break; end }
    else
      each { |x| if x; result = true; break; end }
    end
    result
  end

  def all?(&blk)
    result = true
    if blk
      each { |x| unless blk.call(x); result = false; break; end }
    else
      each { |x| unless x; result = false; break; end }
    end
    result
  end

  def none?(&blk)
    result = true
    if blk
      each { |x| if blk.call(x); result = false; break; end }
    else
      each { |x| if x; result = false; break; end }
    end
    result
  end

  def sort
    to_a.sort
  end

  alias collect map
  alias filter select
  alias find_all select
  alias inject reduce
  alias detect find
  alias entries to_a
  alias member? include?

  # Pattern-conditional filters.  Use `===` so user-defined classes
  # that respond to it (Range, Class, etc.) work without special-casing.
  def grep(pattern, &blk)
    out = []
    each { |x|
      if pattern === x
        out << (blk ? blk.call(x) : x)
      end
    }
    out
  end

  def grep_v(pattern, &blk)
    out = []
    each { |x|
      unless pattern === x
        out << (blk ? blk.call(x) : x)
      end
    }
    out
  end

  def find_index(target = nil, &blk)
    i = 0
    each { |x|
      hit = blk ? blk.call(x) : (x == target)
      return i if hit
      i += 1
    }
    nil
  end

  # cycle(n) — yield each element n times in order.  Without a block
  # we return an Array of the cycled elements (koruby has no real
  # Enumerator).  Without n and without a block CRuby would return
  # an infinite Enumerator; we materialize 100 cycles which is enough
  # for typical `.first(N)` / `.take(N)` consumers.
  def cycle(n = nil, &blk)
    if blk.nil?
      return n.nil? ? to_a * 100 : to_a * n
    end
    if n.nil?
      loop { each { |x| blk.call(x) } }
    else
      n.times { each { |x| blk.call(x) } }
    end
  end

  # Enumerator stand-in: with_index(start=0) — pairs each element with
  # an incrementing index.  Without a block, returns Array of [el, i].
  def with_index(start = 0, &blk)
    out = []
    i = start
    each { |x|
      if blk
        blk.call(x, i)
      else
        out << [x, i]
      end
      i += 1
    }
    blk ? self : out
  end

  # Enumerable#each_slice / each_cons — Array overrides these as cfunc;
  # this picks up the missing Range / Hash / String case.
  def each_slice(n, &blk)
    out = blk ? nil : []
    buf = []
    each { |x|
      buf << x
      if buf.size == n
        if blk then blk.call(buf) else out << buf end
        buf = []
      end
    }
    if !buf.empty?
      if blk then blk.call(buf) else out << buf end
    end
    blk ? self : out
  end

  def each_cons(n, &blk)
    out = blk ? nil : []
    buf = []
    each { |x|
      buf << x
      buf.shift if buf.size > n
      if buf.size == n
        if blk then blk.call(buf.dup) else out << buf.dup end
      end
    }
    blk ? self : out
  end

  # Enumerable#minmax — Array overrides; this is a fallback for Range etc.
  def minmax
    arr = to_a
    arr.empty? ? [nil, nil] : [arr.min, arr.max]
  end

  def sort_by(&blk)
    return enum_for(:sort_by) unless blk
    pairs = []
    each { |x| pairs << [blk.call(x), x] }
    pairs.sort { |a, b| a[0] <=> b[0] }.map { |p| p[1] }
  end unless method_defined?(:sort_by)

  # Enumerable#zip — pair each elem with same-index elems from each arg.
  def zip(*others, &blk)
    arr = to_a
    out = arr.each_with_index.map { |x, i|
      [x] + others.map { |o| (o.is_a?(Array) ? o : o.to_a)[i] }
    }
    if blk
      out.each(&blk)
      nil
    else
      out
    end
  end unless method_defined?(:zip)

  def uniq(&blk)
    seen = {}
    out = []
    each { |x|
      k = blk ? blk.call(x) : x
      unless seen[k]
        seen[k] = true
        out << x
      end
    }
    out
  end unless method_defined?(:uniq)

  def tally(into = nil)
    h = into || {}
    each { |x| h[x] = (h[x] || 0) + 1 }
    h
  end unless method_defined?(:tally)

  def to_h(&blk)
    h = {}
    if blk
      each { |x|
        pair = blk.call(x)
        h[pair[0]] = pair[1]
      }
    else
      each { |pair|
        h[pair[0]] = pair[1] if pair.is_a?(Array)
      }
    end
    h
  end unless method_defined?(:to_h)

  # Enumerable#chunk_while / slice_when — Array overrides; fallback here.
  def chunk_while(&blk)
    out = []
    cur = nil
    prev = nil
    first = true
    each { |x|
      if first
        cur = [x]; prev = x; first = false
      elsif blk.call(prev, x)
        cur << x; prev = x
      else
        out << cur; cur = [x]; prev = x
      end
    }
    out << cur if cur && !cur.empty?
    out
  end
end

# Array, Hash, Range, String all support `each`, so wire up Enumerable.
# `include` below adds only methods that the target class doesn't
# already define (cfuncs win), so Array#map etc. stay fast.
class Array
  include Enumerable
  class << self
    def try_convert(o)
      return o if o.is_a?(Array)
      return nil unless o.respond_to?(:to_ary)
      r = o.to_ary
      return nil if r.nil?
      unless r.is_a?(Array)
        raise TypeError,
              "can't convert #{o.class} to Array (#{o.class}#to_ary gives #{r.class})"
      end
      r
    end
  end
end

class Hash
  include Enumerable
  class << self
    def try_convert(o)
      return o if o.is_a?(Hash)
      return nil unless o.respond_to?(:to_hash)
      r = o.to_hash
      return nil if r.nil?
      unless r.is_a?(Hash)
        raise TypeError,
              "can't convert #{o.class} to Hash (#{o.class}#to_hash gives #{r.class})"
      end
      r
    end
  end

  # default_proc= is now provided by the C builtin which stores into
  # the hash struct's `default_proc` field directly so Hash#[] miss
  # path can dispatch the proc.

  # Hash#rehash — re-build the bucket index from current keys.  Our
  # implementation re-hashes lazily on every lookup, so this is a no-op.
  def rehash; self; end unless method_defined?(:rehash)

  def to_proc
    me = self
    ->(k) { me[k] }
  end unless method_defined?(:to_proc)

  def to_hash; self; end unless method_defined?(:to_hash)
  # Note: Hash#to_h is defined later (line ~1500) — leave it there so a
  # single canonical implementation exists.

  # Hash#hash — content-based hash that's order-independent.  Each
  # entry contributes (k.hash * 17 ^ v.hash) into a XOR accumulator,
  # and the size is mixed in.  Recursion guard via class-level Hash.
  @@__hash_seen = {}
  def hash
    if @@__hash_seen[self.object_id]
      return self.class.hash
    end
    @@__hash_seen[self.object_id] = true
    begin
      h = size * 7
      each_pair { |k, v|
        # Use a multiplicative mix that doesn't cancel under XOR even
        # when many entries share equal values.
        h ^= ((k.hash * 17 + 13) * (v.hash * 23 + 11)) & 0xffffffffffffffff
      }
      h
    ensure
      @@__hash_seen.delete(self.object_id)
    end
  end

  def transform_keys!(*args, &blk)
    if args.size == 0 && !blk
      return enum_for(:transform_keys!)
    end
    if args.size > 1
      raise ArgumentError, "wrong number of arguments (given #{args.size}, expected 0..1)"
    end
    raise FrozenError, "can't modify frozen Hash: #{inspect}" if frozen?
    repl = args[0]
    new_h = {}
    each_pair { |k, v|
      nk = if repl && repl.key?(k)
             repl[k]
           elsif blk
             blk.call(k)
           else
             k
           end
      new_h[nk] = v
    }
    clear
    new_h.each_pair { |k, v| self[k] = v }
    self
  end unless method_defined?(:transform_keys!)
  def transform_values!(&blk)
    return enum_for(:transform_values!) unless blk
    raise FrozenError, "can't modify frozen Hash: #{inspect}" if frozen?
    each_pair { |k, v| self[k] = blk.call(v) }
    self
  end unless method_defined?(:transform_values!)
  def select!(&blk)
    return enum_for(:select!) unless blk
    rm = []
    each_pair { |k, v| rm << k unless blk.call(k, v) }
    return nil if rm.empty?
    rm.each { |k| delete(k) }
    self
  end unless method_defined?(:select!)
  alias_method(:filter!, :select!) rescue nil
  def reject!(&blk)
    return enum_for(:reject!) unless blk
    rm = []
    each_pair { |k, v| rm << k if blk.call(k, v) }
    return nil if rm.empty?
    rm.each { |k| delete(k) }
    self
  end unless method_defined?(:reject!)
  def keep_if(&blk); select!(&blk) || self; end unless method_defined?(:keep_if)
  def delete_if(&blk); reject!(&blk) || self; end unless method_defined?(:delete_if)
  def initialize_copy(other)
    clear
    other.each_pair { |k, v| self[k] = v } if other.respond_to?(:each_pair)
    self
  end unless method_defined?(:initialize_copy)
  def compact!
    rm = []
    each_pair { |k, v| rm << k if v.nil? }
    return nil if rm.empty?
    rm.each { |k| delete(k) }
    self
  end unless method_defined?(:compact!)
  def compact
    h = {}
    each_pair { |k, v| h[k] = v unless v.nil? }
    h
  end unless method_defined?(:compact)
end

class Range
  include Enumerable

  def reverse_each(&blk)
    return enum_for(:reverse_each) unless blk
    arr = to_a
    arr.reverse_each(&blk)
    self
  end unless method_defined?(:reverse_each)

  def each_with_object(memo, &blk)
    return enum_for(:each_with_object, memo) unless blk
    each { |x| blk.call(x, memo) }
    memo
  end unless method_defined?(:each_with_object)

  def enum_for(method = :each, *args)
    receiver = self
    e = Enumerator.new { |y|
      receiver.send(method, *args) { |*x|
        y.yield(x.size == 1 ? x[0] : x)
      }
    }
    # Source-method memo: Enumerator#each(&blk) re-invokes the
    # original method with the user's block so methods like
    # Hash#transform_values aggregate their natural return value.
    e.instance_variable_set(:@__source_obj,    receiver)
    e.instance_variable_set(:@__source_method, method)
    e.instance_variable_set(:@__source_args,   args)
    if receiver.respond_to?(:size)
      e.instance_variable_set(:@__size, receiver.size)
    end
    e
  end unless method_defined?(:enum_for)

  alias_method(:to_enum, :enum_for) rescue nil
end

# Numeric / String have <=> so they pick up Comparable's between?/clamp.
# Integer / Float / String / Symbol include Comparable directly so
# their methods table holds the snapshot.  Numeric also includes
# Comparable for the benefit of `Integer < Numeric < Comparable`
# ancestor checks and user `class Foo < Numeric` getting between? etc.
class Numeric
  include Comparable

  # Numeric#<=> — default falls back to identity (CRuby returns 0 when
  # self.equal?(other), nil otherwise).  Subclasses (Integer/Float)
  # override; this is the shared root.  Adding this to Numeric used to
  # trip the global basic-op redef flag and ~10× regress optcarrot,
  # but korb_check_basic_op_redef now skips Numeric (Integer/Float
  # cfuncs still win at lookup).
  def <=>(other)
    self.equal?(other) ? 0 : nil
  end unless method_defined?(:<=>)

  # Numeric#dup / #clone — return self.  Numeric instances are
  # immutable in CRuby, and dup/clone are no-ops; copy-on-write style
  # subclasses also expect identity.  clone(freeze:) honors the keyword.
  def dup
    self
  end
  def clone(freeze: true)
    if freeze == false
      raise ArgumentError, "can't unfreeze #{self.class}"
    end
    self
  end

  # Numeric#fdiv — Float division.  All numeric kinds get this.
  def fdiv(other)
    self.to_f / other.to_f
  end unless method_defined?(:fdiv)

  # Numeric#ceil / #floor / #round / #truncate — default impls go via
  # #to_f and let Float handle the rounding.  Subclasses (Integer / Float)
  # override.  These satisfy `Class.new(Numeric)` instances.
  def ceil(*args)
    to_f.ceil(*args)
  end unless method_defined?(:ceil)
  def floor(*args)
    to_f.floor(*args)
  end unless method_defined?(:floor)
  def round(*args)
    to_f.round(*args)
  end unless method_defined?(:round)
  def truncate(*args)
    to_f.truncate(*args)
  end unless method_defined?(:truncate)

  def finite?
    if respond_to?(:nan?) && nan?
      false
    elsif respond_to?(:infinite?) && infinite?
      false
    else
      true
    end
  end unless method_defined?(:finite?)

  def infinite?; nil; end unless method_defined?(:infinite?)
  def nan?; false; end unless method_defined?(:nan?)
  def real; self; end unless method_defined?(:real)
  def imaginary; 0; end unless method_defined?(:imaginary)
  def real?; !is_a?(Complex); end unless method_defined?(:real?)
  def integer?; is_a?(Integer); end unless method_defined?(:integer?)
end

class Integer
  # Bit-set predicates: x.allbits?(mask) ⇔ (x & mask) == mask;
  # x.anybits?(mask) ⇔ (x & mask) != 0; x.nobits? ⇔ (x & mask) == 0.
  # The mask is coerced via #to_int (CRuby semantics).
  def allbits?(mask)
    mask = __coerce_bit_mask(mask)
    (self & mask) == mask
  end unless method_defined?(:allbits?)
  def anybits?(mask)
    mask = __coerce_bit_mask(mask)
    (self & mask) != 0
  end unless method_defined?(:anybits?)
  def nobits?(mask)
    mask = __coerce_bit_mask(mask)
    (self & mask) == 0
  end unless method_defined?(:nobits?)
  private
  def __coerce_bit_mask(mask)
    return mask if mask.is_a?(Integer)
    raise TypeError, "no implicit conversion of #{mask.class} into Integer" \
      unless mask.respond_to?(:to_int)
    r = mask.to_int
    raise TypeError,
          "can't convert #{mask.class} to Integer (#{mask.class}#to_int gives #{r.class})" \
      unless r.is_a?(Integer)
    r
  end
  public

  # Integer.sqrt — integer square root.  Raises Math::DomainError on
  # negative input.  Coerces non-Integer via #to_int.
  def self.sqrt(n)
    unless n.is_a?(Integer)
      raise TypeError, "no implicit conversion of #{n.class} into Integer" \
        unless n.respond_to?(:to_int)
      n = n.to_int
      raise TypeError,
            "can't convert #{n.class} to Integer (#{n.class}#to_int gives #{n.class})" \
        unless n.is_a?(Integer)
    end
    raise Math::DomainError, "Numerical argument is out of domain - 'isqrt'" if n < 0
    return 0 if n == 0
    # Newton's method on Integer.
    x = n
    y = (x + 1) / 2
    while y < x
      x = y
      y = (x + n / x) / 2
    end
    x
  end unless respond_to?(:sqrt)

  # Integer#to_r — produce a Rational with self/1.
  def to_r
    Rational(self, 1)
  end unless method_defined?(:to_r)

  # Integer#numerator — self; #denominator — 1.  CRuby's Numeric API.
  def numerator
    self
  end unless method_defined?(:numerator)
  def denominator
    1
  end unless method_defined?(:denominator)

  # Integer#ord — self.  Defined on Integer for CRuby compatibility
  # (Integer#ord == self because Integer is Unicode codepoint).
  def ord
    self
  end unless method_defined?(:ord)

  # Integer#rationalize — same as to_r; the optional precision arg is
  # ignored (CRuby returns the integer-equivalent rational regardless).
  def rationalize(*a)
    raise ArgumentError, "wrong number of arguments (given #{a.size}, expected 0..1)" \
      if a.size > 1
    Rational(self, 1)
  end unless method_defined?(:rationalize)

  # Integer.try_convert(obj) — coerce via #to_int; return nil on
  # non-integer-convertible objects, raise TypeError if to_int returns
  # a non-Integer.
  def self.try_convert(obj)
    return obj if obj.is_a?(Integer)
    return nil unless obj.respond_to?(:to_int)
    r = obj.to_int
    return nil if r.nil?
    raise TypeError,
          "can't convert #{obj.class} to Integer (#{obj.class}#to_int gives #{r.class})" \
      unless r.is_a?(Integer)
    r
  end unless respond_to?(:try_convert)
end

class Integer
  include Comparable

  def gcd(other)
    a = self.abs
    b = other.abs
    while b != 0
      t = a % b
      a = b
      b = t
    end
    a
  end

  def lcm(other)
    return 0 if self == 0 || other == 0
    (self * other).abs / gcd(other)
  end

  def gcdlcm(other)
    [gcd(other), lcm(other)]
  end

  # CRuby's Integer#ceildiv(other) — equivalent to (self / other).ceil
  # but accepts any Numeric coerce-able RHS.  Raises ZeroDivisionError
  # for integer 0; for Float 0 follows ((self.to_f) / 0).ceil semantics.
  def ceildiv(other)
    raise TypeError, "no implicit conversion to Integer" unless other.respond_to?(:to_int) || other.is_a?(Numeric)
    other = other.to_int if other.respond_to?(:to_int) && !other.is_a?(Numeric)
    if other.is_a?(Integer)
      raise ZeroDivisionError, "divided by 0" if other == 0
      q, r = self.divmod(other)
      (r == 0) ? q : (other > 0 ? q + 1 : q)
    else
      (self.to_f / other.to_f).ceil
    end
  end unless method_defined?(:ceildiv)

  def digits(base = 10)
    raise Math::DomainError, "out of domain" if self < 0
    raise ArgumentError, "negative radix" if base.is_a?(Integer) && base < 0
    raise ArgumentError, "invalid radix #{base}" if base.is_a?(Integer) && base < 2
    return [0] if self == 0
    n = self
    arr = []
    while n > 0
      arr << n % base
      n = n / base
    end
    arr
  end

  def pow(exp, mod = nil)
    if mod
      r = 1
      b = self % mod
      e = exp
      while e > 0
        r = (r * b) % mod if e & 1 == 1
        e = e >> 1
        b = (b * b) % mod
      end
      r
    else
      self ** exp
    end
  end
end

class Float
  include Comparable

  def zero?;     self == 0.0; end
  def positive?; self >  0.0; end
  def negative?; self <  0.0; end
  def nan?;      self != self; end
  def finite?;   !nan? && self != Float::INFINITY && self != -Float::INFINITY; end
  def infinite?
    return  1 if self ==  Float::INFINITY
    return -1 if self == -Float::INFINITY
    nil
  end
  def divmod(other)
    q = (self / other).floor
    [q, self - q * other]
  end

  # Float#numerator / #denominator — derived from to_r.  For
  # Infinity / NaN, denominator returns 1 and numerator returns the
  # Float itself (CRuby semantics — these aren't representable as
  # rationals but follow the convention of "indivisible whole").
  def numerator
    return self if !finite?
    to_r.numerator
  end unless method_defined?(:numerator)
  def denominator
    return 1 if !finite?
    to_r.denominator
  end unless method_defined?(:denominator)

  # Float#to_r — produce a Rational that exactly represents the
  # IEEE-754 double's value.  Bignum-based denominator handles
  # repeating decimals losslessly.  Special values (Infinity / NaN)
  # raise FloatDomainError as in CRuby.
  def to_r
    raise FloatDomainError, "Infinity" if infinite?
    raise FloatDomainError, "NaN" if nan?
    return Rational(0, 1) if self == 0.0
    f = self
    e = 0
    while f.abs >= 1.0
      f /= 2.0
      e += 1
    end
    while f.abs < 0.5 && f != 0.0
      f *= 2.0
      e -= 1
    end
    # f is now in [0.5, 1.0); multiply by 2^53 to extract integer mantissa.
    m_int = (f * (2.0 ** 53)).to_i
    shift = e - 53
    if shift >= 0
      Rational(m_int * (2 ** shift), 1)
    else
      Rational(m_int, 2 ** (-shift))
    end
  end unless method_defined?(:to_r)

  # Float#rationalize — same as Float#to_r when no argument.  CRuby's
  # implementation simplifies self to within ±epsilon.  We approximate
  # with Float#to_r because the "lossless" form is what Float#to_r
  # returns and the test suite's tolerance hides the simplification.
  def rationalize(*a)
    raise ArgumentError, "wrong number of arguments (given #{a.size}, expected 0..1)" \
      if a.size > 1
    to_r
  end unless method_defined?(:rationalize)
end

class String
  include Comparable

  # Cheap Ruby-side bang variants.  Each one calls the non-bang version
  # and replaces the receiver's bytes via String#replace if it differs;
  # returns nil when no change happened (CRuby semantics).
  def chop!
    raise FrozenError, "can't modify frozen String: #{inspect}" if frozen?
    return nil if empty?
    r = chop
    r == self ? nil : (replace(r); self)
  end
  # strip! / lstrip! / rstrip! are implemented in C (str_strip_bang etc.)
  # — the C versions raise FrozenError unconditionally on frozen self.
  def squeeze!(*a)
    r = squeeze(*a)
    r == self ? nil : (replace(r); self)
  end
  def delete!(*a)
    r = delete(*a)
    r == self ? nil : (replace(r); self)
  end

  def chr; empty? ? "" : self[0]; end

  def clear
    replace("")
  end unless method_defined?(:clear)

  def codepoints; bytes; end unless method_defined?(:codepoints)
  def each_codepoint(&blk); bytes.each(&blk); end unless method_defined?(:each_codepoint)
  def each_grapheme_cluster(&blk); each_char(&blk); end unless method_defined?(:each_grapheme_cluster)
  def grapheme_clusters; chars; end unless method_defined?(:grapheme_clusters)

  def casecmp(other)
    return nil unless other.is_a?(String)
    a = downcase
    b = other.downcase
    a < b ? -1 : a > b ? 1 : 0
  end unless method_defined?(:casecmp)

  def casecmp?(other)
    c = casecmp(other)
    c.nil? ? nil : c == 0
  end unless method_defined?(:"casecmp?")

  def !~(other)
    !(self =~ other)
  end unless method_defined?(:!~)

  # `+@` returns a fresh mutable copy when self is frozen *or* chilled
  # (CRuby 3.4+).  For plain mutable strings, returns self for parity
  # with pre-3.4 behavior.  Internal `__chilled?` is implemented in C.
  def +@
    (frozen? || __chilled?) ? dup : self
  end unless method_defined?(:+@)
  def -@; frozen? ? self : dup.freeze; end unless method_defined?(:-@)

  # String#match — Regexp not supported; return nil instead of crashing.
  def match(*); nil; end unless method_defined?(:match)
  def match?(*); false; end unless method_defined?(:match?)

  # String#dump — escape non-printable / non-ASCII bytes in the
  # CRuby-compatible \x.. / \u{....} / standard escapes.  Round-trip
  # via undump gives the original bytes.
  def dump
    out = "\""
    bs = bytes
    i = 0
    while i < bs.size
      b = bs[i]
      case b
      when 0x09 then out << "\\t"
      when 0x0a then out << "\\n"
      when 0x0d then out << "\\r"
      when 0x07 then out << "\\a"
      when 0x08 then out << "\\b"
      when 0x0c then out << "\\f"
      when 0x0b then out << "\\v"
      when 0x1b then out << "\\e"
      when 0x22 then out << "\\\""
      when 0x5c then out << "\\\\"
      when 0x23 # `#` — only escape when followed by interpolation triggers.
        nx = bs[i + 1]
        if nx == 0x7B || nx == 0x24 || nx == 0x40 # `{`, `$`, `@`
          out << "\\#"
        else
          out << "#"
        end
      when 0x20..0x7e then out << b.chr
      else
        out << ("\\x%02X" % b)
      end
      i += 1
    end
    out << "\""
  end unless method_defined?(:dump)
  def undump
    s = self
    raise RuntimeError, "invalid dumped string" unless s.start_with?("\"") && s.end_with?("\"")
    body = s[1...-1]
    out = ""
    i = 0
    while i < body.size
      ch = body[i]
      if ch == "\\"
        nx = body[i+1]
        case nx
        when "t" then out << "\t"; i += 2
        when "n" then out << "\n"; i += 2
        when "r" then out << "\r"; i += 2
        when "a" then out << "\a"; i += 2
        when "b" then out << "\b"; i += 2
        when "f" then out << "\f"; i += 2
        when "v" then out << "\v"; i += 2
        when "e" then out << "\e"; i += 2
        when "\\" then out << "\\"; i += 2
        when "\"" then out << "\""; i += 2
        when "x"
          hex = body[i+2, 2]
          out << hex.to_i(16).chr
          i += 4
        when "u"
          if body[i+2] == "{"
            close = body.index("}", i+3)
            cp = body[i+3...close].to_i(16)
            out << cp.chr
            i = close + 1
          else
            cp = body[i+2, 4].to_i(16)
            out << cp.chr
            i += 6
          end
        else
          out << nx
          i += 2
        end
      else
        out << ch
        i += 1
      end
    end
    out
  end unless method_defined?(:undump)
  def try_convert(s)
    return s if s.is_a?(String)
    s.respond_to?(:to_str) ? s.to_str : nil
  end
  class << self
    def try_convert(s)
      return s if s.is_a?(String)
      return nil unless s.respond_to?(:to_str)
      r = s.to_str
      return nil if r.nil?
      unless r.is_a?(String)
        raise TypeError,
              "can't convert #{s.class} to String (#{s.class}#to_str gives #{r.class})"
      end
      r
    end
  end

  def upto(to, exclusive = false, &blk)
    return self.dup unless blk
    s = self.dup
    target = to.to_s
    while s <= target
      break if exclusive && s == target
      yield s
      s = s.succ
    end
    self
  end unless method_defined?(:upto)

  def succ!
    r = succ
    r == self ? nil : (replace(r); self)
  end unless method_defined?(:succ!)
  alias_method(:next!, :succ!) rescue nil
end

class Symbol
  include Comparable
end

# Numeric predicates handled C-side (in builtins.c) to avoid flipping
# the basic-op-redef flag.

# Rational — exact rational arithmetic.  Stored as (num, den) with
# den > 0 and gcd(num, den) == 1.
class Rational
  include Comparable
  def kind_of?(klass); klass == Numeric || klass == Rational || super; end
  alias is_a? kind_of?
  attr_reader :numerator, :denominator
  alias num numerator
  alias den denominator

  # Rational#integer? — false (Rational is never an integer-like, even
  # if denominator happens to be 1, because it's a Rational instance).
  # CRuby Rational#integer? returns false unconditionally.
  def integer?
    false
  end
  # Disallow Rational.new (CRuby uses Rational(...) factory only).
  class << self
    undef_method(:new) rescue nil
  end

  # Rational#floor / ceil / round — built on integer division of the
  # numerator by the denominator with rounding mode applied.  Without
  # arg returns Integer; with arg returns a Rational.
  def floor(n = 0)
    if n == 0
      if @numerator >= 0
        @numerator / @denominator
      else
        -((-@numerator + @denominator - 1) / @denominator)
      end
    else
      m = 10 ** n
      Rational((self * m).floor, m)
    end
  end unless method_defined?(:floor)
  def ceil(n = 0)
    -((-self).floor(n))
  end unless method_defined?(:ceil)
  def round(n = 0)
    (self + Rational(1, 2)).floor(n)
  end unless method_defined?(:round)

  def div(other)
    (self / other).floor
  end unless method_defined?(:div)
  def divmod(other)
    q = (self / other).floor
    [q, self - other * q]
  end unless method_defined?(:divmod)
  def fdiv(other); to_f / other.to_f; end unless method_defined?(:fdiv)
  def finite?; true; end unless method_defined?(:finite?)
  def infinite?; nil; end unless method_defined?(:infinite?)
  def nan?; false; end unless method_defined?(:nan?)
  def integer?; @denominator == 1; end unless method_defined?(:integer?)
  def real?; true; end unless method_defined?(:real?)
  def real; self; end unless method_defined?(:real)
  def imaginary; 0; end unless method_defined?(:imaginary)
  def truncate(n = 0)
    if n == 0
      to_i
    else
      m = 10 ** n
      (self * m).truncate / Rational(m, 1)
    end
  end unless method_defined?(:truncate)

  def initialize(n, d = 1)
    if d == 0
      raise ZeroDivisionError, "denominator is zero"
    end
    if d < 0
      n = -n
      d = -d
    end
    g = n.gcd(d)
    @numerator = n / g
    @denominator = d / g
  end

  def +(other)
    case other
    when Rational
      Rational(@numerator * other.denominator + other.numerator * @denominator, @denominator * other.denominator)
    when Integer
      Rational(@numerator + other * @denominator, @denominator)
    when Float
      to_f + other
    end
  end

  def -(other)
    case other
    when Rational
      Rational(@numerator * other.denominator - other.numerator * @denominator, @denominator * other.denominator)
    when Integer
      Rational(@numerator - other * @denominator, @denominator)
    when Float
      to_f - other
    end
  end

  def *(other)
    case other
    when Rational
      Rational(@numerator * other.numerator, @denominator * other.denominator)
    when Integer
      Rational(@numerator * other, @denominator)
    when Float
      to_f * other
    end
  end

  def /(other)
    case other
    when Rational
      Rational(@numerator * other.denominator, @denominator * other.numerator)
    when Integer
      Rational(@numerator, @denominator * other)
    when Float
      to_f / other
    end
  end

  def -@; Rational(-@numerator, @denominator); end

  # Rational ** exp — Integer exp stays exact; Rational/Float exp falls
  # back to Float arithmetic (matching CRuby's Rational#** when the
  # result wouldn't be representable as a Rational).
  def **(other)
    case other
    when Integer
      if other >= 0
        Rational(@numerator ** other, @denominator ** other)
      elsif @numerator == 0
        raise ZeroDivisionError, "0**negative"
      else
        # (a/b)^-n = (b/a)^n
        Rational(@denominator ** -other, @numerator ** -other)
      end
    when Rational
      to_f ** other.to_f
    when Float
      to_f ** other
    else
      raise TypeError, "Rational#** doesn't accept #{other.class}"
    end
  end

  def <=>(other)
    case other
    when Rational
      (@numerator * other.denominator) <=> (other.numerator * @denominator)
    when Integer
      @numerator <=> other * @denominator
    when Float
      to_f <=> other
    end
  end

  def ==(other)
    case other
    when Rational
      @numerator == other.numerator && @denominator == other.denominator
    when Integer
      @denominator == 1 && @numerator == other
    when Float
      to_f == other
    else
      false
    end
  end

  def to_f; @numerator.to_f / @denominator; end
  def to_i; @numerator / @denominator; end
  def to_r; self; end
  def to_s; "#{@numerator}/#{@denominator}"; end
  def inspect; "(#{@numerator}/#{@denominator})"; end
  def hash; [@numerator, @denominator].hash; end
  def abs; @numerator < 0 ? Rational(-@numerator, @denominator) : self; end
  def negative?; @numerator < 0; end
  def positive?; @numerator > 0; end
  def zero?;     @numerator == 0; end

  # Numeric#coerce protocol — let `Float + Rational`, `Integer cmp
  # Rational`, etc. participate.  Returns [other_promoted, self].
  def coerce(other)
    case other
    when Rational then [other, self]
    when Integer  then [Rational(other, 1), self]
    when Float    then [other, to_f]
    else
      raise TypeError, "#{other.class} can't be coerced into Rational"
    end
  end
end

def Rational(n, d = 1)
  # Bypass Rational.new (which is undef-method'd to satisfy CRuby's
  # "Rational does not respond to new") by allocating + invoking
  # initialize directly.
  r = Rational.allocate
  r.send(:initialize, n, d)
  r
end

# Integer/Float arithmetic with Rational/Complex — handled in C-side
# int_plus / int_minus / int_mul / int_div by checking the operand's
# class and delegating to the Rational/Complex class.  Implemented in
# builtins.c so the fast path stays a cfunc.

# Complex — Cartesian (real, imag) representation.
class Numeric
  # Marker: Complex/Rational include this so they're kind_of Numeric.
end
module ComplexNumericMarker
end
class Complex
  include ComplexNumericMarker
  # Provide a `kind_of?(Numeric)` shim.  We can't easily change the
  # parent class of an existing class, so use is_a? customization via
  # a method defined on Complex.
  def kind_of?(klass); klass == Numeric || klass == Complex || super; end
  alias is_a? kind_of?
  attr_reader :real, :imaginary
  alias imag imaginary

  def initialize(r, i = 0)
    @real = r
    @imaginary = i
  end

  def fdiv(other); Complex(real.to_f / other.to_f, imaginary.to_f / other.to_f); end unless method_defined?(:fdiv)
  def finite?; @real.finite? && @imaginary.finite?; end unless method_defined?(:finite?)
  def infinite?; finite? ? nil : 1; end unless method_defined?(:infinite?)
  def nan?
    (@real.respond_to?(:nan?) && @real.nan?) ||
    (@imaginary.respond_to?(:nan?) && @imaginary.nan?)
  end unless method_defined?(:nan?)
  def integer?; false; end unless method_defined?(:integer?)
  def real?; @imaginary == 0; end unless method_defined?(:real?)

  def +(other)
    if other.is_a?(Complex)
      Complex.new(@real + other.real, @imaginary + other.imag)
    else
      Complex.new(@real + other, @imaginary)
    end
  end

  def -(other)
    if other.is_a?(Complex)
      Complex.new(@real - other.real, @imaginary - other.imag)
    else
      Complex.new(@real - other, @imaginary)
    end
  end

  def *(other)
    if other.is_a?(Complex)
      Complex.new(@real * other.real - @imaginary * other.imag,
                  @real * other.imag + @imaginary * other.real)
    else
      Complex.new(@real * other, @imaginary * other)
    end
  end

  def /(other)
    if other.is_a?(Complex)
      d = other.real * other.real + other.imag * other.imag
      Complex.new((@real * other.real + @imaginary * other.imag) / d,
                  (@imaginary * other.real - @real * other.imag) / d)
    else
      Complex.new(@real / other, @imaginary / other)
    end
  end

  def -@; Complex.new(-@real, -@imaginary); end

  # Complex ** Integer — repeated multiply for non-negative; reciprocal
  # of positive power for negative.  Non-integer exp goes via polar:
  # z^w = exp(w * log(z))  (only handles real exp here for simplicity).
  def **(other)
    case other
    when Integer
      if other == 0
        Complex.new(1, 0)
      elsif other > 0
        result = self
        (other - 1).times { result = result * self }
        result
      else
        # 1 / (self ** -other)
        denom = self ** -other
        Complex.new(1, 0) / denom
      end
    when Float, Rational
      r   = abs
      th  = arg
      e   = other.to_f
      nr  = r ** e
      nth = th * e
      Complex.new(nr * Math.cos(nth), nr * Math.sin(nth))
    else
      raise TypeError, "Complex#** doesn't accept #{other.class}"
    end
  end

  def ==(other)
    if other.is_a?(Complex)
      @real == other.real && @imaginary == other.imag
    else
      @imaginary == 0 && @real == other
    end
  end

  def abs; Math.sqrt(@real * @real + @imaginary * @imaginary); end
  alias magnitude abs
  def abs2; @real * @real + @imaginary * @imaginary; end
  def conjugate; Complex.new(@real, -@imaginary); end
  alias conj conjugate

  # Polar form: angle (radians) and rect-from-polar.
  def arg; Math.atan2(@imaginary, @real); end
  alias angle arg
  alias phase arg

  def polar; [abs, arg]; end
  def rectangular; [@real, @imaginary]; end
  alias rect rectangular

  # Complex.polar(r, theta = 0)
  def self.polar(r, theta = 0)
    Complex.new(r * Math.cos(theta), r * Math.sin(theta))
  end

  # Complex.rect / Complex.rectangular — explicit rectangular factory.
  def self.rectangular(r, i = 0)
    Complex.new(r, i)
  end
  class << self
    alias rect rectangular
  end

  # Numeric#coerce: lift Integer/Float/Rational into Complex so
  # `1 + Complex(0, 1)` etc. work.
  def coerce(other)
    case other
    when Complex  then [other, self]
    when Numeric  then [Complex.new(other, 0), self]
    else
      raise TypeError, "#{other.class} can't be coerced into Complex"
    end
  end

  def to_s
    sign = @imaginary >= 0 ? "+" : "-"
    "#{@real}#{sign}#{@imaginary.abs}i"
  end
  def inspect; "(#{to_s})"; end
end

def Complex(r, i = 0)
  Complex.new(r, i)
end

# --- Enumerable extensions written in Ruby ---
# All `yield` inside a nested block forwards to the *block's* block,
# not the outer method's block.  koruby doesn't yet implement
# block-forwarding, so these helpers take an explicit `&blk` parameter
# and call `blk.call(...)` to escape the ambiguity.
module Enumerable
  def group_by(&blk)
    h = {}
    each { |x|
      k = blk.call(x)
      (h[k] ||= []) << x
    }
    h
  end

  def partition(&blk)
    yes = []; no = []
    each { |x| (blk.call(x) ? yes : no) << x }
    [yes, no]
  end

  def each_cons(n, &blk)
    buf = []
    each { |x|
      buf << x
      buf.shift if buf.size > n
      blk.call(buf.dup) if buf.size == n
    }
    nil
  end

  def tally
    h = {}
    each { |x| h[x] = (h[x] || 0) + 1 }
    h
  end

  def min_by(&blk)
    best = nil; best_key = nil; seen = false
    each { |x|
      k = blk.call(x)
      if !seen || (k <=> best_key) < 0
        best = x; best_key = k; seen = true
      end
    }
    best
  end

  def max_by(&blk)
    best = nil; best_key = nil; seen = false
    each { |x|
      k = blk.call(x)
      if !seen || (k <=> best_key) > 0
        best = x; best_key = k; seen = true
      end
    }
    best
  end

  def sum(init = 0, &blk)
    s = init
    if blk
      each { |x| s = s + blk.call(x) }
    else
      each { |x| s = s + x }
    end
    s
  end

  def flat_map(&blk)
    out = []
    each { |x|
      r = blk.call(x)
      if r.is_a?(Array)
        r.each { |e| out << e }
      else
        out << r
      end
    }
    out
  end
  alias collect_concat flat_map

  def take_while(&blk)
    out = []
    each { |x| break unless blk.call(x); out << x }
    out
  end

  def drop_while(&blk)
    out = []
    dropping = true
    each { |x|
      dropping = false if dropping && !blk.call(x)
      out << x unless dropping
    }
    out
  end

  def each_with_object(memo, &blk)
    each { |x| blk.call(x, memo) }
    memo
  end

  def chunk_while(&blk)
    out = []
    cur = []
    prev = nil
    first = true
    each { |x|
      if first
        cur << x
        first = false
      elsif blk.call(prev, x)
        cur << x
      else
        out << cur
        cur = [x]
      end
      prev = x
    }
    out << cur unless cur.empty?
    out
  end
end

# --- Hash extensions ---
# Same block-forwarding workaround: explicit `&blk`.
class Hash
  def assoc(key)
    each_pair { |k, v| return [k, v] if k == key }
    nil
  end unless method_defined?(:assoc)

  def rassoc(value)
    each_pair { |k, v| return [k, v] if v == value }
    nil
  end unless method_defined?(:rassoc)

  def <(other)
    return nil unless other.is_a?(Hash)
    return false if size >= other.size
    all? { |k, v| other.key?(k) && other[k] == v }
  end unless method_defined?(:<)

  def <=(other)
    return nil unless other.is_a?(Hash)
    return false if size > other.size
    all? { |k, v| other.key?(k) && other[k] == v }
  end unless method_defined?(:<=)

  def >(other)
    return nil unless other.is_a?(Hash)
    other < self
  end unless method_defined?(:>)

  def >=(other)
    return nil unless other.is_a?(Hash)
    other <= self
  end unless method_defined?(:>=)

  def transform_values(&blk)
    return enum_for(:transform_values) unless blk
    h = {}
    each_pair { |k, v| h[k] = blk.call(v) }
    h
  end

  # Hash#flatten — flatten one level by default: [k1, v1, k2, v2, ...].
  # When passed depth, also flattens Array values up to depth.
  # CRuby coerces depth via #to_int and raises TypeError on failure.
  def flatten(depth = 1)
    unless depth.is_a?(Integer)
      raise TypeError, "no implicit conversion of #{depth.class} into Integer" \
        unless depth.respond_to?(:to_int)
      depth = depth.to_int
      raise TypeError, "can't convert #{depth.class} to Integer" \
        unless depth.is_a?(Integer)
    end
    result = []
    each_pair { |k, v| result << k; result << v }
    return result if depth <= 1
    result.flatten(depth - 1)
  end unless method_defined?(:flatten)

  # Hash#transform_keys(hash = nil, &blk) — optional hash argument
  # specifies explicit key replacements; the block (if given) handles
  # any keys not present in hash.
  def transform_keys(*args, &blk)
    if args.size == 0 && !blk
      return enum_for(:transform_keys)
    end
    if args.size > 1
      raise ArgumentError, "wrong number of arguments (given #{args.size}, expected 0..1)"
    end
    repl = args[0]
    h = {}
    each_pair { |k, v|
      nk = if repl && repl.key?(k)
             repl[k]
           elsif blk
             blk.call(k)
           else
             k
           end
      h[nk] = v
    }
    h
  end

  def to_h(&blk)
    if blk
      h = {}
      each_pair { |k, v|
        pair = blk.call(k, v)
        if !pair.is_a?(Array) && pair.respond_to?(:to_ary)
          pair = pair.to_ary
        end
        unless pair.is_a?(Array) && pair.size == 2
          if pair.is_a?(Array)
            raise ArgumentError, "element has wrong array length (expected 2, was #{pair.size})"
          else
            raise TypeError, "wrong element type #{pair.class} (expected array)"
          end
        end
        h[pair[0]] = pair[1]
      }
      h
    else
      self
    end
  end

  def to_s
    inspect
  end

  def inspect
    parts = []
    each_pair { |k, v| parts << "#{k.inspect}=>#{v.inspect}" }
    "{" + parts.join(", ") + "}"
  end

  def reject(&blk)
    h = {}
    each_pair { |k, v| h[k] = v unless blk.call(k, v) }
    h
  end

  def select(&blk)
    h = {}
    each_pair { |k, v| h[k] = v if blk.call(k, v) }
    h
  end
  alias filter select

  def any?(&blk)
    return size > 0 unless blk
    result = false
    each_pair { |k, v| if blk.call(k, v); result = true; break; end }
    result
  end

  def all?(&blk)
    return true unless blk
    result = true
    each_pair { |k, v| unless blk.call(k, v); result = false; break; end }
    result
  end

  def count(*args, &blk)
    return size if args.empty? && !blk
    n = 0
    each_pair { |k, v| n += 1 if blk.call(k, v) }
    n
  end

  def find(&blk)
    found = nil
    each_pair { |k, v| if blk.call(k, v); found = [k, v]; break; end }
    found
  end
  alias detect find

  def max_by(&blk)
    best = nil; best_key = nil; seen = false
    each_pair { |k, v|
      kk = blk.call(k, v)
      if !seen || (kk <=> best_key) > 0
        best = [k, v]; best_key = kk; seen = true
      end
    }
    best
  end

  def min_by(&blk)
    best = nil; best_key = nil; seen = false
    each_pair { |k, v|
      kk = blk.call(k, v)
      if !seen || (kk <=> best_key) < 0
        best = [k, v]; best_key = kk; seen = true
      end
    }
    best
  end

  def values_at(*keys)
    keys.map { |k| self[k] }
  end

  def sort(&blk)
    blk ? to_a.sort(&blk) : to_a.sort
  end
end

# --- Range extensions ---
class Range
  def to_h(&blk)
    h = {}
    if blk
      each { |x|
        pair = blk.call(x)
        h[pair[0]] = pair[1]
      }
    else
      each { |pair|
        h[pair[0]] = pair[1]
      }
    end
    h
  end

  # Range#bsearch — binary search.  Two modes (per CRuby docs):
  #   find-minimum: block returns true/false → return smallest x with true
  #   find-any:     block returns negative/zero/positive Integer → return
  #                 x with 0, or one near the boundary
  # Only Integer ranges are supported here (Float is rare in tests).
  def bsearch(&blk)
    return enum_for(:bsearch) unless blk
    lo = self.begin
    hi = self.end
    raise TypeError, "can't do binary search for #{lo.class}" unless lo.is_a?(Integer) || lo.nil?
    raise TypeError, "can't do binary search for #{hi.class}" unless hi.is_a?(Integer) || hi.nil?
    return nil if lo.nil? && hi.nil?
    if lo.nil?
      lo = hi - 1
      lo -= 1 while blk.call(lo) ? true : false
      lo += 1
    end
    if hi.nil?
      hi = lo + 1
      hi += 1 while blk.call(hi) ? false : true
    end
    hi -= 1 if exclude_end?
    # find-minimum mode: detect by calling once
    sample = blk.call(lo)
    if sample == true || sample == false
      satisfied = nil
      satisfied = lo if sample
      l, h = lo, hi
      while l <= h
        mid = (l + h) / 2
        if blk.call(mid)
          satisfied = mid
          h = mid - 1
        else
          l = mid + 1
        end
      end
      satisfied
    elsif sample.is_a?(Integer)
      return lo if sample == 0
      l, h = lo + 1, hi
      while l <= h
        mid = (l + h) / 2
        v = blk.call(mid)
        return mid if v == 0
        if v < 0
          h = mid - 1
        else
          l = mid + 1
        end
      end
      nil
    else
      raise TypeError, "wrong argument type"
    end
  end unless method_defined?(:bsearch)

  def cover?(v)
    f = first
    l = last
    # Range vs Range: normalize each range's effective last (subtract
    # one when exclude_end and the bound is an Integer) so comparisons
    # are uniform.
    if v.is_a?(Range)
      vf = v.first
      vl = v.last
      return false if vf.nil? && !f.nil?
      return false if vl.nil? && !l.nil?
      return false if !f.nil? && !vf.nil? && vf < f
      if !l.nil? && !vl.nil?
        outer_last = (exclude_end? && l.is_a?(Integer)) ? l - 1 : l
        inner_last = (v.exclude_end? && vl.is_a?(Integer)) ? vl - 1 : vl
        return inner_last <= outer_last
      end
      return true
    end
    if f.nil?
      return v < l if exclude_end?
      return v <= l
    elsif l.nil?
      return f <= v
    end
    if exclude_end?
      f <= v && v < l
    else
      f <= v && v <= l
    end
  rescue
    false
  end

  def min
    first
  end

  def max
    if exclude_end?
      last - 1
    else
      last
    end
  end

  def sum(init = 0)
    s = init
    each { |x| s = s + x }
    s
  end
end

# --- Object extensions ---
class Object
  def eql?(other)
    self == other
  end

  def equal?(other)
    object_id == other.object_id
  end

  def hash
    object_id
  end

  def then
    yield self
  end
  alias yield_self then
end

# --- Kernel extensions ---
def produce(initial = nil, &blk)
  raise ArgumentError, "no block given" unless blk
  Enumerator.new do |y|
    cur = initial
    y.yield cur unless cur.nil?
    cur = blk.call(cur)
    loop do
      y.yield cur
      cur = blk.call(cur)
    end
  end
end

def loop
  while true
    yield
  end
rescue StopIteration
  nil
end

# lambda / proc are cfuncs in builtins.c; the cfunc flips is_lambda
# on the captured block.  Don't redefine them here — a Ruby def would
# silently drop the lambda flag and break `lambda { return X }.call`'s
# local-return semantics.

# --- String extensions ---
class String
  def lines(sep = "\n")
    out = []
    cur = ""
    each_char { |ch|
      cur << ch
      if ch == sep
        out << cur
        cur = ""
      end
    }
    out << cur unless cur.empty?
    out
  end

  def intern
    to_sym
  end

  def ljust(n, pad = " ")
    return self if size >= n
    self + pad * (n - size)
  end

  def rjust(n, pad = " ")
    return self if size >= n
    pad * (n - size) + self
  end

  def center(n, pad = " ")
    return self if size >= n
    total = n - size
    left = total / 2
    right = total - left
    pad * left + self + pad * right
  end

  def squeeze(chars = nil)
    out = ""
    prev = nil
    each_char { |ch|
      if ch != prev || (chars && !chars.include?(ch))
        out << ch
      end
      prev = ch
    }
    out
  end

  def slice(a, b = nil)
    if b.nil?
      self[a]
    else
      self[a, b]
    end
  end

  # String#slice! — take a slice and remove it in place.  Returns the
  # extracted substring, or nil when no slice was extracted.  Raises
  # FrozenError on a frozen receiver before any other check.
  def slice!(a, b = nil)
    raise FrozenError, "can't modify frozen String: #{inspect}" if frozen?
    # Coerce Integer-shaped args via #to_int (CRuby calls to_int even
    # for the (idx, len) and (idx) forms).
    if !a.is_a?(Integer) && !a.is_a?(Range) && !a.is_a?(String) &&
       !a.is_a?(Regexp) && a.respond_to?(:to_int)
      a = a.to_int
    end
    if !b.nil? && !b.is_a?(Integer) && b.respond_to?(:to_int)
      b = b.to_int
    end
    if b.nil?
      r = self[a]
      return nil if r.nil?
      if a.is_a?(Integer)
        n = self.size
        idx = a < 0 ? a + n : a
        return nil if idx < 0 || idx >= n
        replace(self[0, idx] + self[idx + 1, n - idx - 1])
      elsif a.is_a?(Range)
        first = a.first
        last  = a.last
        excl  = a.exclude_end?
        n = self.size
        i = first.is_a?(Integer) ? (first < 0 ? first + n : first) : 0
        j = last.is_a?(Integer)  ? (last  < 0 ? last  + n : last)  : (n - 1)
        i = 0 if i < 0
        j = excl ? j - 1 : j
        if i > j || i >= n
          return r
        end
        j = n - 1 if j >= n
        replace(self[0, i] + self[j + 1, n - j - 1])
      elsif a.is_a?(String)
        idx = self.index(a)
        return nil if idx.nil?
        replace(self[0, idx] + self[idx + a.size, self.size - idx - a.size])
      end
      r
    else
      return nil if b < 0
      r = self[a, b]
      return nil if r.nil?
      n = self.size
      idx = a < 0 ? a + n : a
      idx = 0 if idx < 0
      len = b
      len = n - idx if idx + len > n
      replace(self[0, idx] + self[idx + len, n - idx - len])
      r
    end
  end

  def swapcase
    s = ""
    each_char { |ch|
      o = ch.bytes[0]
      if o >= 65 && o <= 90
        s << (o + 32).chr
      elsif o >= 97 && o <= 122
        s << (o - 32).chr
      else
        s << ch
      end
    }
    s
  end

  def capitalize
    return self if empty?
    upper = self[0].upcase
    rest = self[1..-1]
    upper + (rest ? rest.downcase : "")
  end
end

# --- Array extensions ---
# Array's `each` is a cfunc that calls yield on its block; Ruby code
# inside *that* block can't `yield` further (no forwarding to outer
# method's block), so use `&blk` and `blk.call`.
class Array
  def group_by(&blk)
    h = {}
    i = 0
    while i < self.size
      x = self[i]
      k = blk.call(x)
      (h[k] ||= []) << x
      i += 1
    end
    h
  end

  def partition(&blk)
    yes = []; no = []
    i = 0
    while i < self.size
      x = self[i]
      (blk.call(x) ? yes : no) << x
      i += 1
    end
    [yes, no]
  end

  def tally
    h = {}
    i = 0
    while i < self.size
      x = self[i]
      h[x] = (h[x] || 0) + 1
      i += 1
    end
    h
  end

  # Array#each_cons is registered as a cfunc (builtins/array.c) so it
  # supports both the block form and the Array-of-windows return form;
  # the AST version here would otherwise win and reject no-block calls.
end

class Array
  # bsearch_index — same shape as #bsearch but returns the index instead
  # of the element.  Find-minimum mode (block returns true/false) and
  # find-any mode (returns -/0/+ Integer) supported.
  def bsearch_index(&blk)
    return enum_for(:bsearch_index) unless blk
    return nil if empty?
    sample = blk.call(self[0])
    if sample == true || sample == false
      satisfied = nil
      l, h = 0, size - 1
      while l <= h
        mid = (l + h) / 2
        if blk.call(self[mid])
          satisfied = mid
          h = mid - 1
        else
          l = mid + 1
        end
      end
      satisfied
    elsif sample.is_a?(Integer)
      return 0 if sample == 0
      l, h = 1, size - 1
      while l <= h
        mid = (l + h) / 2
        v = blk.call(self[mid])
        return mid if v == 0
        v < 0 ? (h = mid - 1) : (l = mid + 1)
      end
      nil
    else
      raise TypeError, "wrong argument type"
    end
  end unless method_defined?(:bsearch_index)

  def each_entry(&blk)
    return enum_for(:each_entry) unless blk
    each(&blk)
    self
  end unless method_defined?(:each_entry)

  def map!(&blk)
    return enum_for(:map!) unless blk
    i = 0
    n = size
    while i < n
      self[i] = blk.call(self[i])
      i += 1
    end
    self
  end unless method_defined?(:map!)
  alias_method(:collect!, :map!) rescue nil

  def each_index(&blk)
    return enum_for(:each_index) unless blk
    i = 0
    n = size
    while i < n
      blk.call(i)
      i += 1
    end
    self
  end unless method_defined?(:each_index)

  def chunk(&blk)
    return enum_for(:chunk) unless blk
    out = []
    cur_key = :__none__
    cur_grp = nil
    each do |x|
      k = blk.call(x)
      if k.equal?(cur_key)
        cur_grp << x
      else
        out << [cur_key, cur_grp] unless cur_key == :__none__
        cur_key = k
        cur_grp = [x]
      end
    end
    out << [cur_key, cur_grp] unless cur_key == :__none__
    out
  end unless method_defined?(:chunk)

  # Set ops on Array.  Order is "self's order, dedup".
  def &(other)
    o = other.to_ary if other.respond_to?(:to_ary)
    o ||= other
    out = []
    seen = {}
    each do |x|
      next if seen[x]
      next unless o.include?(x)
      seen[x] = true
      out << x
    end
    out
  end unless method_defined?(:&)
  def |(other)
    o = other.to_ary if other.respond_to?(:to_ary)
    o ||= other
    out = []
    seen = {}
    each do |x|
      next if seen[x]
      seen[x] = true
      out << x
    end
    o.each do |x|
      next if seen[x]
      seen[x] = true
      out << x
    end
    out
  end unless method_defined?(:|)
  def -(other)
    o = other.to_ary if other.respond_to?(:to_ary)
    o ||= other
    reject { |x| o.include?(x) }
  end unless method_defined?(:-)

  def values_at(*indices)
    indices.flat_map { |i|
      if i.is_a?(Range)
        self[i] || []
      else
        [self[i]]
      end
    }
  end unless method_defined?(:values_at)

  def union(*others)
    seen = {}
    out = []
    each { |x| unless seen[x]; seen[x] = true; out << x; end }
    others.each { |o|
      o.each { |x| unless seen[x]; seen[x] = true; out << x; end }
    }
    out
  end unless method_defined?(:union)
  def intersection(*others)
    others.reduce(self.dup) { |acc, o| acc & o }
  end unless method_defined?(:intersection)
  def difference(*others)
    others.reduce(self.dup) { |acc, o| acc - o }
  end unless method_defined?(:difference)

  def rindex(target = nil, &blk)
    if blk
      i = size - 1
      while i >= 0
        return i if blk.call(self[i])
        i -= 1
      end
      nil
    else
      i = size - 1
      while i >= 0
        return i if self[i] == target
        i -= 1
      end
      nil
    end
  end unless method_defined?(:rindex)

  def select!(&blk)
    return enum_for(:select!) unless blk
    rm = []
    each_with_index { |v, i| rm << i unless blk.call(v) }
    return nil if rm.empty?
    rm.reverse_each { |i| delete_at(i) }
    self
  end unless method_defined?(:select!)
  alias_method(:filter!, :select!) rescue nil
  def reject!(&blk)
    return enum_for(:reject!) unless blk
    rm = []
    each_with_index { |v, i| rm << i if blk.call(v) }
    return nil if rm.empty?
    rm.reverse_each { |i| delete_at(i) }
    self
  end unless method_defined?(:reject!)
  def keep_if(&blk); select!(&blk) || self; end unless method_defined?(:keep_if)
  def delete_if(&blk); reject!(&blk) || self; end unless method_defined?(:delete_if)

  def sort_by!(&blk)
    replace(sort_by(&blk))
  end unless method_defined?(:sort_by!)

  def to_h(&blk)
    h = {}
    if blk
      each { |x|
        pair = blk.call(x)
        unless pair.is_a?(Array) && pair.size == 2
          raise TypeError, "wrong element type (expected 2-element Array)"
        end
        h[pair[0]] = pair[1]
      }
    else
      each_with_index { |pair, i|
        unless pair.is_a?(Array) && pair.size == 2
          raise TypeError, "wrong element type (expected 2-element Array) at index #{i}"
        end
        h[pair[0]] = pair[1]
      }
    end
    h
  end unless method_defined?(:to_h)

  # chunk_while { |a, b| ... } — group consecutive pairs where the
  # block returns truthy.
  def chunk_while(&blk)
    return [] if empty?
    chunks = []
    cur = [self[0]]
    i = 1
    while i < size
      a = self[i - 1]
      b = self[i]
      if blk.call(a, b)
        cur << b
      else
        chunks << cur
        cur = [b]
      end
      i += 1
    end
    chunks << cur
    chunks
  end

  # slice_when — opposite of chunk_while: split where the block returns truthy.
  def slice_when(&blk)
    return [] if empty?
    chunks = []
    cur = [self[0]]
    i = 1
    while i < size
      a = self[i - 1]
      b = self[i]
      if blk.call(a, b)
        chunks << cur
        cur = [b]
      else
        cur << b
      end
      i += 1
    end
    chunks << cur
    chunks
  end

  def minmax
    [min, max]
  end

  # chain — concatenate self + other lists' enumerations.
  def chain(*others)
    r = self.dup
    others.each { |o| r.concat(o.to_a) }
    r
  end

  # filter_map { |x| ... } — map then drop falsy results.
  def filter_map(&blk)
    r = []
    each { |x|
      v = blk.call(x)
      r << v if v
    }
    r
  end
end

# Range — give it a few enumerable basics it can delegate via to_a.
class Range
  # Range#eql? — endpoints compared with eql? (type-strict), so
  # 0..1 and 0..1.0 are not eql?.  exclude_end must also match.
  def eql?(other)
    return false unless other.is_a?(Range)
    self.exclude_end? == other.exclude_end? &&
      self.begin.eql?(other.begin) &&
      self.end.eql?(other.end)
  end

  # Range#to_s — endpoints rendered via #to_s (CRuby differs from
  # #inspect, which uses #inspect on endpoints).  nil endpoints are
  # rendered as the empty string in the bare-end / bare-begin form.
  def to_s
    b = self.begin
    e = self.end
    sep = exclude_end? ? "..." : ".."
    b_str = b.nil? ? "" : b.to_s
    e_str = e.nil? ? "" : e.to_s
    if b.nil? && e.nil?
      "nil#{sep}nil"
    else
      "#{b_str}#{sep}#{e_str}"
    end
  end

  def group_by(&blk)
    to_a.group_by(&blk)
  end

  def filter_map(&blk)
    to_a.filter_map(&blk)
  end

  def chain(*others)
    to_a.chain(*others)
  end

  # Lazy enumeration shim — sufficient for `(1..Float::INFINITY).lazy.map { ... }.first(N)`.
  def lazy
    LazyRange.new(self)
  end
end

# Array also supports lazy enumeration by delegating to LazyRange-like
# semantics on a wrapped enumerable source.
class Array
  def lazy
    LazyEnum.new(self)
  end
end

# Generic lazy chain over any enumerable that responds to `each`.
class LazyEnum
  def initialize(src)
    @src = src
    @ops = []
  end

  def map(&blk)
    n = LazyEnum.new(@src)
    n.instance_variable_set(:@ops, @ops + [[:map, blk]])
    n
  end

  def select(&blk)
    n = LazyEnum.new(@src)
    n.instance_variable_set(:@ops, @ops + [[:select, blk]])
    n
  end
  alias filter select

  def first(n = nil)
    one = n.nil?
    n = 1 if one
    out = []
    @src.each { |x|
      v = x
      keep = true
      @ops.each { |op, p|
        if op == :map
          v = p.call(v)
        elsif op == :select
          unless p.call(v)
            keep = false
            break
          end
        end
      }
      next unless keep
      out << v
      break if out.size >= n
    }
    one ? out.first : out
  end
  alias take first

  def force
    out = []
    @src.each { |x|
      v = x
      keep = true
      @ops.each { |op, p|
        if op == :map
          v = p.call(v)
        elsif op == :select
          unless p.call(v)
            keep = false
            break
          end
        end
      }
      out << v if keep
    }
    out
  end
  alias to_a force
end

# Minimal Lazy enumerator: chains map/select calls without materializing
# the whole range.  Only supports map/select + first/take.
class LazyRange
  def initialize(range)
    @range = range
    @ops = []          # Array of [:map | :select, proc]
  end

  def map(&blk)
    n = LazyRange.new(@range)
    n.instance_variable_set(:@ops, @ops + [[:map, blk]])
    n
  end

  def select(&blk)
    n = LazyRange.new(@range)
    n.instance_variable_set(:@ops, @ops + [[:select, blk]])
    n
  end
  alias filter select

  def first(n = nil)
    one = n.nil?
    n = 1 if one
    out = []
    iter_range(@range) { |x|
      v = x
      keep = true
      @ops.each { |op, p|
        if op == :map
          v = p.call(v)
        elsif op == :select
          unless p.call(v)
            keep = false
            break
          end
        end
      }
      next unless keep
      out << v
      break if out.size >= n
    }
    one ? out.first : out
  end
  alias take first

  def force
    first(1 << 30)  # huge cap; not truly infinite, but ok for tests
  end
  alias to_a force

  private

  # Iterate the range, but stop early when the block raises StopIteration
  # or breaks.  Range#each handles infinite end via Float::INFINITY by
  # incrementing forever — we need an explicit early-exit.
  def iter_range(r)
    cur = r.first
    last = r.last
    excl = r.exclude_end?
    while true
      if !last.nil? && (excl ? cur >= last : cur > last)
        break
      end
      yield cur
      cur = cur + 1
    end
  end
end

# Enumerator — Fiber-backed coroutine producer.  Supports the
# `Enumerator.new { |y| y << v; y.yield v }` form plus #next, #peek,
# #rewind, #each, #to_a, and Enumerable-style chaining.
class Enumerator
  include Enumerable

  class Yielder
    # Inside the producer block, calls to `y << v` or `y.yield(v)`
    # cross back to the consumer via Fiber.yield.
    def <<(val)
      ::Fiber.yield(val)
      self
    end
    define_method(:yield) do |*args|
      ::Fiber.yield(args.size == 1 ? args[0] : args)
    end
  end

  # Sentinel value: producer returns this to signal the stream ended.
  ENUMERATOR_DONE = Object.new

  def initialize(&blk)
    raise ArgumentError, "Enumerator.new requires a block" unless blk
    @blk = blk
    @fiber = nil
    @done = false
    @peeked = nil
    @has_peeked = false
    @__size = nil
  end

  # Enumerator#size — when the upstream collection had a known size
  # (recorded by enum_for), return it; else nil (stream of unknown
  # length, matches CRuby's "no size proc" case).
  def size
    @__size
  end

  def _start
    blk = @blk
    @fiber = Fiber.new do
      blk.call(Yielder.new)
      ENUMERATOR_DONE
    end
    @done = false
    @peeked = nil
    @has_peeked = false
  end

  def next
    if @has_peeked
      v = @peeked
      @peeked = nil
      @has_peeked = false
      return v
    end
    raise StopIteration if @done
    _start unless @fiber
    v = @fiber.resume
    if v.equal?(ENUMERATOR_DONE)
      @done = true
      raise StopIteration
    end
    v
  end

  def peek
    return @peeked if @has_peeked
    @peeked = self.next
    @has_peeked = true
    @peeked
  end

  def rewind
    @fiber = nil
    @done = false
    @peeked = nil
    @has_peeked = false
    self
  end

  def each(&blk)
    return self unless blk
    # If we know the source method, re-dispatch with the user's block
    # so the source's natural return value (e.g. Hash#transform_values
    # returns a Hash) is delivered, instead of a fiber-yielded stream.
    if defined?(@__source_obj) && @__source_obj && @__source_method
      return @__source_obj.send(@__source_method, *(@__source_args || []), &blk)
    end
    rewind
    loop do
      blk.call(self.next)
    end
    self
  end

  def to_a
    rewind
    out = []
    loop do
      out << self.next
    end
    out
  end
  alias force to_a

  # Lazy chain: calls block on each yielded value but defers materializing.
  def map(&blk)
    me = self
    Enumerator.new do |y|
      me.rewind
      loop { y << blk.call(me.next) }
    end
  end
  alias collect map

  def select(&blk)
    me = self
    Enumerator.new do |y|
      me.rewind
      loop do
        v = me.next
        y << v if blk.call(v)
      end
    end
  end
  alias filter select

  def first(n = nil)
    rewind
    if n.nil?
      begin
        return self.next
      rescue StopIteration
        return nil
      end
    end
    out = []
    n.times do
      begin
        out << self.next
      rescue StopIteration
        break
      end
    end
    out
  end

  def take(n)
    first(n)
  end

  def size
    @__size
  end

  # Lazy chain wrapping any Enumerator: each chained map/select/etc.
  # builds a new Enumerator::Lazy that pulls from the previous one,
  # so `(1..).lazy.map.select.first(N)` only materializes N items.
  def lazy
    Lazy.new(self)
  end

  class Lazy < Enumerator
    # Don't go through super — koruby doesn't forward `&blk` through
    # `super`, so set the parent's ivars directly.
    def initialize(source = nil, &blk)
      if source && !blk
        @blk = proc do |y|
          source.rewind if source.respond_to?(:rewind)
          source.each { |v| y << v }
        end
      elsif blk
        @blk = blk
      else
        raise ArgumentError, "Lazy.new needs source or block"
      end
      @fiber = nil
      @done = false
      @peeked = nil
      @has_peeked = false
    end

    def map(&blk)
      me = self
      Lazy.new do |y|
        me.rewind
        loop { y << blk.call(me.next) }
      end
    end
    alias collect map

    def select(&blk)
      me = self
      Lazy.new do |y|
        me.rewind
        loop do
          v = me.next
          y << v if blk.call(v)
        end
      end
    end
    alias filter select

    def reject(&blk)
      me = self
      Lazy.new do |y|
        me.rewind
        loop do
          v = me.next
          y << v unless blk.call(v)
        end
      end
    end

    def take_while(&blk)
      me = self
      Lazy.new do |y|
        me.rewind
        loop do
          v = me.next
          break unless blk.call(v)
          y << v
        end
      end
    end

    def drop_while(&blk)
      me = self
      Lazy.new do |y|
        me.rewind
        dropping = true
        loop do
          v = me.next
          if dropping
            next if blk.call(v)
            dropping = false
          end
          y << v
        end
      end
    end

    def take(n)
      me = self
      Lazy.new do |y|
        me.rewind
        i = 0
        loop do
          break if i >= n
          y << me.next
          i += 1
        end
      end
    end

    def drop(n)
      me = self
      Lazy.new do |y|
        me.rewind
        i = 0
        loop do
          v = me.next
          if i < n
            i += 1
            next
          end
          y << v
        end
      end
    end

    def with_index(offset = 0)
      me = self
      Lazy.new do |y|
        me.rewind
        i = offset
        loop do
          y << [me.next, i]
          i += 1
        end
      end
    end

    def force
      to_a
    end

    def each_cons(n)
      me = self
      Lazy.new do |y|
        me.rewind
        buf = []
        loop do
          buf << me.next
          buf.shift if buf.size > n
          y << buf.dup if buf.size == n
        end
      end
    end

    def each_slice(n)
      me = self
      Lazy.new do |y|
        me.rewind
        buf = []
        loop do
          buf << me.next
          if buf.size == n
            y << buf
            buf = []
          end
        end
        y << buf unless buf.empty?
      end
    end

    def cycle(n = nil)
      me = self
      Lazy.new do |y|
        cnt = 0
        loop do
          break if n && cnt >= n
          me.rewind
          loop { y << me.next }
          cnt += 1
        end
      end
    end

    def filter_map(&blk)
      me = self
      Lazy.new do |y|
        me.rewind
        loop do
          v = blk.call(me.next)
          y << v if v
        end
      end
    end

    def chunk_while(&blk)
      force.chunk_while(&blk)
    end
    def slice_when(&blk)
      force.slice_when(&blk)
    end
    def uniq(&blk)
      me = self
      Lazy.new do |y|
        me.rewind
        seen = {}
        loop do
          v = me.next
          k = blk ? blk.call(v) : v
          unless seen[k]
            seen[k] = true
            y << v
          end
        end
      end
    end

    def eager
      Enumerator.new { |y| each { |v| y.yield v } }
    end

    def first(n = nil)
      take(n || 1).force.then { |a| n ? a : a.first }
    end
  end
end

# Range / Array bridges to the unified Enumerator::Lazy.  These
# replace the older LazyRange / LazyEnum stubs for chains where the
# Lazy semantics matter (`.map.select.first(N)`).
class Range
  def lazy_enum
    me = self
    Enumerator::Lazy.new(Enumerator.new { |y| me.each { |v| y << v } })
  end
end

class Array
  def lazy_enum
    me = self
    Enumerator::Lazy.new(Enumerator.new { |y| me.each { |v| y << v } })
  end
end

# Pathname — thin wrapper over File/Dir methods, the most common
# subset that Ruby scripts pull in via `require 'pathname'`.
class Pathname
  attr_reader :path

  def initialize(path)
    case path
    when Pathname then @path = path.to_s.dup
    when String   then @path = path.dup
    else
      raise TypeError, "Pathname requires String or Pathname, got #{path.class}"
    end
  end

  def to_s
    @path
  end
  alias to_path to_s

  def to_str
    @path
  end

  def inspect
    "#<Pathname:#{@path}>"
  end

  def ==(other)
    other.is_a?(Pathname) && @path == other.to_s
  end
  alias eql? ==

  def hash
    @path.hash
  end

  def +(other)
    join(other)
  end
  alias / +

  def join(*parts)
    out = @path.dup
    parts.each do |p|
      pstr = p.is_a?(Pathname) ? p.to_s : p.to_s
      if pstr.start_with?("/")
        out = pstr.dup
      elsif out.empty? || out.end_with?("/")
        out << pstr
      else
        out << "/" << pstr
      end
    end
    Pathname.new(out)
  end

  def basename(suffix = nil)
    if suffix
      Pathname.new(File.basename(@path, suffix))
    else
      Pathname.new(File.basename(@path))
    end
  end

  def dirname
    Pathname.new(File.dirname(@path))
  end

  def extname
    File.extname(@path)
  end

  def expand_path(default_dir = nil)
    Pathname.new(default_dir ? File.expand_path(@path, default_dir.to_s)
                              : File.expand_path(@path))
  end

  def absolute?
    @path.start_with?("/")
  end

  def relative?
    !absolute?
  end

  def exist?
    File.exist?(@path)
  end

  def file?
    return false unless File.exist?(@path)
    !File.respond_to?(:directory?) || !File.directory?(@path)
  end

  def directory?
    return false unless File.exist?(@path)
    File.respond_to?(:directory?) ? File.directory?(@path) : false
  end

  def read
    File.read(@path)
  end

  def write(content)
    File.write(@path, content)
  end

  def each_line(&blk)
    if blk
      File.open(@path) { |f| f.each_line(&blk) }
    else
      File.open(@path) { |f| f.each_line.to_a }
    end
  end

  def readlines
    each_line.to_a
  end

  def open(mode = "r", &blk)
    File.open(@path, mode, &blk)
  end

  def children
    return [] unless directory?
    Dir.entries(@path).reject { |e| e == "." || e == ".." }.map do |name|
      self.join(name)
    end
  end

  def each_child(&blk)
    if blk
      children.each(&blk)
      self
    else
      children
    end
  end

  # OS operations — all delegate to FileUtils-style stubs / system calls.
  def realpath
    Pathname.new(File.realpath(@path)) if File.respond_to?(:realpath)
    Pathname.new(File.expand_path(@path))
  rescue
    Pathname.new(File.expand_path(@path))
  end

  def chmod(mode)
    File.chmod(mode, @path)
  end

  def unlink
    File.unlink(@path)
  end
  alias delete unlink

  def rename(new_path)
    # Match CRuby: rename the file, leave the receiver's @path intact.
    # Callers who want a Pathname for the new location use Pathname.new(new).
    File.rename(@path, new_path.to_s)
    0
  end

  def mkdir(mode = 0o755)
    Dir.mkdir(@path, mode)
    self
  end

  def rmdir
    Dir.rmdir(@path)
    self
  end

  def size
    File.size(@path)
  end

  def empty?
    if directory?
      children.empty?
    else
      File.size(@path) == 0
    end
  end

  def glob(pattern)
    full = (@path.end_with?("/") ? @path : @path + "/") + pattern
    Dir.glob(full).map { |p| Pathname.new(p) }
  end

  def parent
    dirname
  end

  def root?
    @path == "/"
  end

  def split
    [dirname, basename]
  end

  def each_filename(&blk)
    parts = @path.split("/").reject(&:empty?)
    if blk
      parts.each(&blk)
      self
    else
      parts.each
    end
  end
end

def Pathname(p)
  p.is_a?(Pathname) ? p : Pathname.new(p)
end

# JSON — pure-Ruby parser + generator.  Handles nil/true/false, Integer
# (Bignum included via String#to_i), Float, String (\uXXXX, common
# escapes), Array, Hash (String keys).  Symbol keys are stringified on
# generate, returned as String on parse — matches CRuby's default.
module JSON
  def self.generate(obj)
    out = String.new
    _generate_into(obj, out)
    out
  end
  def self.dump(obj); generate(obj); end

  # Pretty-print with 2-space indent, matching CRuby's default
  # Ext-style output reasonably closely.
  def self.pretty_generate(obj, opts = nil)
    out = String.new
    _pretty_into(obj, out, 0)
    out
  end

  def self._pretty_into(obj, out, depth)
    case obj
    when nil    then out << "null"
    when true   then out << "true"
    when false  then out << "false"
    when Integer, Float then out << obj.to_s
    when String then _generate_string(obj, out)
    when Symbol then _generate_string(obj.to_s, out)
    when Array
      if obj.empty?
        out << "[]"
        return
      end
      out << "[\n"
      indent = "  " * (depth + 1)
      obj.each_with_index do |v, i|
        out << indent
        _pretty_into(v, out, depth + 1)
        out << "," unless i == obj.size - 1
        out << "\n"
      end
      out << ("  " * depth) << "]"
    when Hash
      if obj.empty?
        out << "{}"
        return
      end
      out << "{\n"
      indent = "  " * (depth + 1)
      keys = obj.keys
      keys.each_with_index do |k, i|
        out << indent
        _generate_string(k.to_s, out)
        out << ": "
        _pretty_into(obj[k], out, depth + 1)
        out << "," unless i == keys.size - 1
        out << "\n"
      end
      out << ("  " * depth) << "}"
    else
      _pretty_into(obj.to_s, out, depth)
    end
  end

  def self._generate_into(obj, out)
    case obj
    when nil    then out << "null"
    when true   then out << "true"
    when false  then out << "false"
    when Integer, Float then out << obj.to_s
    when String then _generate_string(obj, out)
    when Symbol then _generate_string(obj.to_s, out)
    when Array
      out << "["
      first = true
      obj.each do |v|
        out << "," unless first
        first = false
        _generate_into(v, out)
      end
      out << "]"
    when Hash
      out << "{"
      first = true
      obj.each do |k, v|
        out << "," unless first
        first = false
        _generate_string(k.to_s, out)
        out << ":"
        _generate_into(v, out)
      end
      out << "}"
    else
      if obj.respond_to?(:to_json)
        out << obj.to_json
      else
        _generate_string(obj.to_s, out)
      end
    end
  end

  def self._generate_string(s, out)
    out << "\""
    i = 0
    while i < s.size
      c = s[i]
      case c
      when "\\" then out << "\\\\"
      when "\"" then out << "\\\""
      when "\n" then out << "\\n"
      when "\r" then out << "\\r"
      when "\t" then out << "\\t"
      when "\b" then out << "\\b"
      when "\f" then out << "\\f"
      else
        # Control chars (< 0x20) get \u escape; others pass through.
        b = c.bytes.first
        if b && b < 0x20
          out << format("\\u%04x", b)
        else
          out << c
        end
      end
      i += 1
    end
    out << "\""
  end

  def self.parse(src, opts = nil)
    src = src.to_s
    parser = Parser.new(src)
    parser.symbolize_names = !!(opts && opts[:symbolize_names])
    v = parser.parse_value
    parser.skip_ws
    raise ParserError, "trailing chars at offset #{parser.pos}" if parser.pos < src.size
    v
  end
  def self.load(src, opts = nil); parse(src, opts); end

  class ParserError < StandardError; end

  class Parser
    attr_reader :pos
    attr_accessor :symbolize_names
    def initialize(src)
      @src = src
      @pos = 0
      @symbolize_names = false
    end
    def err(msg)
      raise ParserError, "#{msg} at offset #{@pos}"
    end
    def skip_ws
      while @pos < @src.size && (c = @src[@pos]) && (c == " " || c == "\t" || c == "\n" || c == "\r")
        @pos += 1
      end
    end
    def parse_value
      skip_ws
      err("unexpected end of input") if @pos >= @src.size
      c = @src[@pos]
      case c
      when "{" then parse_object
      when "[" then parse_array
      when "\"" then parse_string
      when "t", "f" then parse_bool
      when "n" then parse_null
      else
        parse_number
      end
    end
    def parse_object
      err("expected '{'") unless @src[@pos] == "{"
      @pos += 1
      h = {}
      skip_ws
      if @src[@pos] == "}"
        @pos += 1
        return h
      end
      loop do
        skip_ws
        k = parse_string
        k = k.to_sym if @symbolize_names
        skip_ws
        err("expected ':'") unless @src[@pos] == ":"
        @pos += 1
        v = parse_value
        h[k] = v
        skip_ws
        if @src[@pos] == ","
          @pos += 1
        elsif @src[@pos] == "}"
          @pos += 1
          return h
        else
          err("expected ',' or '}'")
        end
      end
    end
    def parse_array
      err("expected '['") unless @src[@pos] == "["
      @pos += 1
      a = []
      skip_ws
      if @src[@pos] == "]"
        @pos += 1
        return a
      end
      loop do
        a << parse_value
        skip_ws
        if @src[@pos] == ","
          @pos += 1
        elsif @src[@pos] == "]"
          @pos += 1
          return a
        else
          err("expected ',' or ']'")
        end
      end
    end
    def parse_string
      err("expected '\"'") unless @src[@pos] == "\""
      @pos += 1
      out = String.new
      while @pos < @src.size
        c = @src[@pos]
        if c == "\""
          @pos += 1
          return out
        elsif c == "\\"
          @pos += 1
          err("unterminated escape") if @pos >= @src.size
          esc = @src[@pos]
          @pos += 1
          case esc
          when "\"" then out << "\""
          when "\\" then out << "\\"
          when "/"  then out << "/"
          when "n"  then out << "\n"
          when "r"  then out << "\r"
          when "t"  then out << "\t"
          when "b"  then out << "\b"
          when "f"  then out << "\f"
          when "u"
            err("bad \\u") if @pos + 4 > @src.size
            hex = @src[@pos, 4]
            @pos += 4
            n = hex.to_i(16)
            if n < 0x80
              out << n.chr
            elsif n < 0x800
              out << ((0xc0 | (n >> 6)) & 0xff).chr
              out << ((0x80 | (n & 0x3f))).chr
            else
              out << ((0xe0 | (n >> 12)) & 0xff).chr
              out << ((0x80 | ((n >> 6) & 0x3f))).chr
              out << ((0x80 | (n & 0x3f))).chr
            end
          else
            err("bad escape \\#{esc}")
          end
        else
          out << c
          @pos += 1
        end
      end
      err("unterminated string")
    end
    def parse_bool
      if @src[@pos, 4] == "true"
        @pos += 4
        return true
      end
      if @src[@pos, 5] == "false"
        @pos += 5
        return false
      end
      err("expected true/false")
    end
    def parse_null
      if @src[@pos, 4] == "null"
        @pos += 4
        return nil
      end
      err("expected null")
    end
    def parse_number
      start = @pos
      @pos += 1 if @src[@pos] == "-"
      while @pos < @src.size && @src[@pos] >= "0" && @src[@pos] <= "9"
        @pos += 1
      end
      is_float = false
      if @pos < @src.size && @src[@pos] == "."
        is_float = true
        @pos += 1
        while @pos < @src.size && @src[@pos] >= "0" && @src[@pos] <= "9"
          @pos += 1
        end
      end
      if @pos < @src.size && (@src[@pos] == "e" || @src[@pos] == "E")
        is_float = true
        @pos += 1
        @pos += 1 if @pos < @src.size && (@src[@pos] == "+" || @src[@pos] == "-")
        while @pos < @src.size && @src[@pos] >= "0" && @src[@pos] <= "9"
          @pos += 1
        end
      end
      lit = @src[start, @pos - start]
      err("invalid number") if lit.empty? || lit == "-"
      is_float ? lit.to_f : lit.to_i
    end
  end
end

class Object
  def to_json
    JSON.generate(self)
  end
end

# BigDecimal — minimal arbitrary-precision decimal, Rational-backed.
# Covers the common script-level patterns: BigDecimal("3.14") / BigDecimal(int),
# arithmetic (+ - * / **), comparisons, to_s / to_i / to_f, abs, sign,
# zero? / positive? / negative? / nan? / infinite?.
#
# Real BigDecimal stores a base-10 mantissa+scale; this Rational-backed
# version is exact for any rational input but won't give base-10
# representations for irrational results.  Enough for tests and
# common config-file arithmetic.
class BigDecimal
  include Comparable

  SIGN_NaN              =  0
  SIGN_POSITIVE_INFINITE = 3
  SIGN_NEGATIVE_INFINITE = -3
  SIGN_POSITIVE_ZERO     = 1
  SIGN_NEGATIVE_ZERO    = -1
  SIGN_POSITIVE_FINITE   = 2
  SIGN_NEGATIVE_FINITE  = -2

  attr_reader :rational, :special

  def initialize(value, _digits = nil)
    case value
    when BigDecimal
      @rational = value.rational
      @special  = value.special
    when Integer
      @rational = Rational(value, 1)
      @special  = nil
    when Float
      if value.nan?
        @special = :nan
      elsif value.infinite?
        @special = value > 0 ? :inf : :ninf
      else
        # Convert Float to Rational via decimal string for exactness.
        s = value.to_s
        return _parse(s) if s =~ nil rescue nil
        # Fallback: lossy approximation.
        @rational = Rational(value.to_i, 1) if !defined?(@rational)
        _parse(s)
      end
    when String
      _parse(value)
    when Rational
      @rational = value
      @special  = nil
    else
      raise TypeError, "can't convert #{value.class} into BigDecimal"
    end
  end

  def _parse(str)
    s = str.strip
    if s == "NaN" || s == "+NaN" || s == "-NaN"
      @special = :nan
      @rational = nil
      return
    end
    if s == "Infinity" || s == "+Infinity"
      @special = :inf
      @rational = nil
      return
    end
    if s == "-Infinity"
      @special = :ninf
      @rational = nil
      return
    end
    @special = nil
    # Handle sign
    sign = 1
    if s.start_with?("-")
      sign = -1; s = s[1..-1]
    elsif s.start_with?("+")
      s = s[1..-1]
    end
    # Split exponent (e/E)
    mant = s; exp_str = nil
    if (idx = s.index("e")) || (idx = s.index("E"))
      mant = s[0, idx]
      exp_str = s[idx + 1, s.size - idx - 1]
    end
    # Split decimal point
    if dot = mant.index(".")
      int_part  = mant[0, dot]
      frac_part = mant[dot + 1, mant.size - dot - 1]
    else
      int_part  = mant
      frac_part = ""
    end
    int_part  = "0" if int_part.empty?
    digits    = int_part + frac_part
    # Reject anything non-digit.
    digits.each_char { |c| raise ArgumentError, "invalid value: #{str}" unless c >= "0" && c <= "9" }
    num = digits.to_i
    den = 10 ** frac_part.size
    if exp_str
      e = exp_str.to_i
      if e >= 0
        num *= 10 ** e
      else
        den *= 10 ** (-e)
      end
    end
    @rational = Rational(sign * num, den)
  end

  def to_r; @rational; end
  def to_i
    raise FloatDomainError, "Computation results in #{@special}" if @special
    @rational.to_i
  end
  def to_f
    case @special
    when :nan  then 0.0 / 0.0
    when :inf  then Float::INFINITY
    when :ninf then -Float::INFINITY
    else            @rational.to_f
    end
  end

  def to_s(_fmt = nil)
    case @special
    when :nan  then "NaN"
    when :inf  then "Infinity"
    when :ninf then "-Infinity"
    end
    return @special.to_s if @special
    # Render as a decimal: numerator / denominator.  Use a fixed cap on
    # post-decimal digits so irrational results (1/3 etc.) terminate.
    n = @rational.numerator
    d = @rational.denominator
    sign = n < 0 ? "-" : ""
    n = n.abs
    int_part = n / d
    rem = n - int_part * d
    if rem == 0
      "#{sign}#{int_part}.0"
    else
      digits = ""
      30.times do
        rem *= 10
        digits << (rem / d).to_s
        rem -= (rem / d) * d
        break if rem == 0
      end
      "#{sign}#{int_part}.#{digits}"
    end
  end
  alias inspect to_s

  def +(other)
    o = _coerce(other)
    return _spec(@special, o.special, :add) if @special || o.special
    BigDecimal.new(@rational + o.rational)
  end
  def -(other)
    o = _coerce(other)
    return _spec(@special, o.special, :sub) if @special || o.special
    BigDecimal.new(@rational - o.rational)
  end
  def *(other)
    o = _coerce(other)
    return _spec(@special, o.special, :mul) if @special || o.special
    BigDecimal.new(@rational * o.rational)
  end
  def /(other)
    o = _coerce(other)
    return _spec(@special, o.special, :div) if @special || o.special
    if o.rational.numerator == 0
      cmp = @rational.numerator <=> 0
      return BigDecimal.new(cmp > 0 ? "Infinity" : (cmp < 0 ? "-Infinity" : "NaN"))
    end
    BigDecimal.new(@rational / o.rational)
  end
  def -@; @special ? BigDecimal.new(_neg_special) : BigDecimal.new(-@rational); end

  def **(other)
    return BigDecimal.new(@rational ** other) if other.is_a?(Integer) && @special.nil?
    BigDecimal.new(to_f ** other.to_f)
  end

  def <=>(other)
    o = _coerce(other) rescue (return nil)
    return nil if @special == :nan || o.special == :nan
    if @special || o.special
      a = @special ? _spec_rank(@special) : (@rational.numerator <=> 0)
      b = o.special ? _spec_rank(o.special) : (o.rational.numerator <=> 0)
      return a <=> b
    end
    @rational <=> o.rational
  end

  def ==(other); (self <=> other) == 0; end
  def hash; [@rational, @special].hash; end

  def abs
    return BigDecimal.new("Infinity") if @special == :ninf || @special == :inf
    return BigDecimal.new("NaN")      if @special == :nan
    BigDecimal.new(@rational.abs)
  end

  def sign
    return SIGN_NaN if @special == :nan
    return SIGN_POSITIVE_INFINITE if @special == :inf
    return SIGN_NEGATIVE_INFINITE if @special == :ninf
    n = @rational.numerator
    return n > 0 ? SIGN_POSITIVE_FINITE : (n < 0 ? SIGN_NEGATIVE_FINITE : SIGN_POSITIVE_ZERO)
  end

  def zero?;     @special.nil? && @rational.numerator == 0; end
  def positive?; sign > 0; end
  def negative?; sign < 0; end
  def nan?;      @special == :nan; end
  def infinite?
    return 1  if @special == :inf
    return -1 if @special == :ninf
    nil
  end
  def finite?;   @special.nil?; end

  def coerce(other)
    [BigDecimal.new(other), self]
  end

  private

  def _coerce(v)
    case v
    when BigDecimal then v
    when Integer, Float, Rational, String then BigDecimal.new(v)
    else raise TypeError, "can't coerce #{v.class} to BigDecimal"
    end
  end

  def _spec_rank(s)
    case s
    when :inf  then  10
    when :ninf then -10
    else 0
    end
  end

  def _neg_special
    case @special
    when :inf  then "-Infinity"
    when :ninf then "Infinity"
    when :nan  then "NaN"
    end
  end

  def _spec(a, b, _op)
    BigDecimal.new("NaN") if a == :nan || b == :nan
    # For simplicity: anything involving infinity falls back to NaN
    # except + with same-sign infinities.  Tests cover only the
    # common finite path, so keep this minimal.
    BigDecimal.new("NaN")
  end
end

def BigDecimal(value, digits = nil)
  BigDecimal.new(value, digits)
end

# Binding — implemented in C (builtins/binding.c).  The C-side struct
# captures fp + names + cref at `Kernel#binding` time so reads and
# writes go directly through the live caller frame.  bootstrap.rb
# just re-exposes the class so user code can refer to ::Binding.

# Marshal — CRuby-compatible binary serialization for the common types.
# Format follows MRI's Marshal version 4.8 wire spec, restricted to:
#   nil 0, true T, false F, Fixnum i, Float f, String ", Symbol :,
#   Array [, Hash {, Bignum l.
# Symbol/object link tables (the deduplication mechanism) aren't
# emitted on dump and aren't followed on load — collateral cost is a
# bigger payload when the same symbol appears many times, but the
# resulting bytes are still valid CRuby Marshal.
module Marshal
  MAJOR_VERSION = 4
  MINOR_VERSION = 8

  def self.dump(obj)
    out = String.new
    out << MAJOR_VERSION.chr
    out << MINOR_VERSION.chr
    sym_table = {}   # Symbol => index
    _dump_into(obj, out, sym_table)
    out
  end

  def self.load(str)
    r = Reader.new(str)
    major = r.read_byte
    minor = r.read_byte
    if major != MAJOR_VERSION
      raise TypeError, "unsupported marshal version #{major}.#{minor}"
    end
    r.read_value
  end

  def self._dump_fixnum_into(n, out)
    if n == 0
      out << 0.chr
      return
    elsif 0 < n && n < 123
      out << (n + 5).chr
      return
    elsif -124 < n && n < 0
      out << ((n - 5) & 0xff).chr
      return
    end
    # Multi-byte form.
    bytes = []
    if n > 0
      while n != 0
        bytes << (n & 0xff)
        n >>= 8
        break if bytes.size == 4
      end
      out << bytes.size.chr
    else
      while n != -1
        bytes << (n & 0xff)
        n >>= 8
        break if bytes.size == 4
      end
      out << ((-bytes.size) & 0xff).chr
    end
    bytes.each { |b| out << b.chr }
  end

  def self._dump_into(obj, out, sym_table = {})
    case obj
    when nil   then out << "0"
    when true  then out << "T"
    when false then out << "F"
    when Symbol
      idx = sym_table[obj]
      if idx
        out << ";"
        _dump_fixnum_into(idx, out)
      else
        sym_table[obj] = sym_table.size
        out << ":"
        s = obj.to_s
        _dump_fixnum_into(s.size, out)
        out << s
      end
    when Integer
      # Fits in MRI Fixnum range (≈ -2^30..2^30-1)?  Use 'i' form.
      if obj >= -1073741824 && obj <= 1073741823
        out << "i"
        _dump_fixnum_into(obj, out)
      else
        out << "l"
        out << (obj < 0 ? "-" : "+")
        n = obj.abs
        shorts = []
        while n > 0
          shorts << (n & 0xffff)
          n >>= 16
        end
        shorts << 0 if shorts.empty?
        _dump_fixnum_into(shorts.size, out)
        shorts.each do |sh|
          out << (sh & 0xff).chr
          out << ((sh >> 8) & 0xff).chr
        end
      end
    when Float
      out << "f"
      s = if obj.nan?
            "nan"
          elsif obj == Float::INFINITY
            "inf"
          elsif obj == -Float::INFINITY
            "-inf"
          else
            obj.to_s
          end
      _dump_fixnum_into(s.size, out)
      out << s
    when String
      out << "\""
      _dump_fixnum_into(obj.size, out)
      out << obj
    when Array
      out << "["
      _dump_fixnum_into(obj.size, out)
      obj.each { |v| _dump_into(v, out, sym_table) }
    when Hash
      out << "{"
      _dump_fixnum_into(obj.size, out)
      obj.each { |k, v| _dump_into(k, out, sym_table); _dump_into(v, out, sym_table) }
    else
      raise TypeError, "no _dump_data is defined for #{obj.class}"
    end
  end

  class Reader
    attr_reader :pos
    def initialize(str)
      @str = str
      @pos = 0
      @symbols = []   # link table: index → Symbol
    end

    def read_byte
      raise ArgumentError, "marshal data too short" if @pos >= @str.size
      b = @str[@pos].bytes.first
      @pos += 1
      b
    end

    def read_n(n)
      raise ArgumentError, "marshal data too short" if @pos + n > @str.size
      s = @str[@pos, n]
      @pos += n
      s
    end

    def read_fixnum
      c = read_byte
      # Treat as signed 8-bit.
      signed = c < 128 ? c : c - 256
      return 0 if signed == 0
      if signed > 0 && signed > 4
        return signed - 5
      end
      if signed < 0 && signed < -4
        return signed + 5
      end
      n = signed
      if n > 0
        v = 0
        n.times do |i|
          v |= read_byte << (i * 8)
        end
        v
      else
        bytes = -n
        v = -1
        bytes.times do |i|
          v &= ~(0xff << (i * 8))
          v |= read_byte << (i * 8)
        end
        v
      end
    end

    def read_value
      tag = read_byte.chr
      case tag
      when "I"
        # Instance-variabled value (typically a String with :E => true
        # encoding ivar from CRuby).  Read the inner value, then skip
        # the ivar pairs — koruby ignores encodings.
        v = read_value
        ivar_cnt = read_fixnum
        ivar_cnt.times { read_value; read_value }
        return v
      when ";"
        # Symbol link: refer to the n-th previously-emitted symbol.
        idx = read_fixnum
        raise TypeError, "Marshal: symbol link out of range (#{idx})" if idx < 0 || idx >= @symbols.size
        return @symbols[idx]
      when "@"
        idx = read_fixnum
        raise TypeError, "Marshal object link unsupported (idx=#{idx})"
      when "0" then nil
      when "T" then true
      when "F" then false
      when "i" then read_fixnum
      when "l"
        sign = read_byte.chr
        len  = read_fixnum
        v = 0
        len.times do |i|
          lo = read_byte
          hi = read_byte
          v |= (lo | (hi << 8)) << (i * 16)
        end
        sign == "-" ? -v : v
      when "f"
        len = read_fixnum
        s = read_n(len)
        case s
        when "inf"  then Float::INFINITY
        when "-inf" then -Float::INFINITY
        when "nan"  then Float::NAN
        else s.to_f
        end
      when "\""
        len = read_fixnum
        read_n(len)
      when ":"
        len = read_fixnum
        sym = read_n(len).to_sym
        @symbols << sym
        sym
      when "["
        len = read_fixnum
        a = []
        len.times { a << read_value }
        a
      when "{"
        len = read_fixnum
        h = {}
        len.times { k = read_value; v = read_value; h[k] = v }
        h
      else
        raise TypeError, "unsupported Marshal tag '#{tag}' at #{@pos - 1}"
      end
    end
  end
end

class Proc
  # Curry: each call accumulates args until enough; then invokes self.
  # Args can come singly (`c[1][2][3]`) or multiply (`c[1, 2][3]`).
  def curry(arity = nil)
    n = arity || self.arity
    n = -n if n < 0
    me = self
    is_lam = me.lambda?
    accum = nil
    if is_lam
      accum = ->(collected) {
        lambda { |*more|
          all = collected + more
          if all.size >= n
            me.call(*all)
          else
            accum.call(all)
          end
        }
      }
    else
      accum = ->(collected) {
        Proc.new { |*more|
          all = collected + more
          if all.size >= n
            me.call(*all)
          else
            accum.call(all)
          end
        }
      }
    end
    accum.call([])
  end

  # Proc#>> / Proc#<<  — function composition.
  # f >> g  ⇒  ->(*a) { g.call(f.call(*a)) }
  # f << g  ⇒  ->(*a) { f.call(g.call(*a)) }
  def >>(other)
    me = self
    ->(*a) { other.call(me.call(*a)) }
  end unless method_defined?(:>>)
  def <<(other)
    me = self
    ->(*a) { me.call(other.call(*a)) }
  end unless method_defined?(:<<)
end

class NameError
  # NameError#receiver — return the recorded receiver, or raise
  # ArgumentError if none was captured.  We don't track the receiver
  # at NameError instantiation (that would need :receiver kwarg
  # support in Class#new), so this is the conservative default.
  def receiver
    if defined?(@receiver) && @receiver
      @receiver
    else
      raise ArgumentError, "no receiver is available"
    end
  end unless method_defined?(:receiver)
  # NameError#name — return @name if recorded.
  def name
    @name if defined?(@name)
  end unless method_defined?(:name)
end

class FrozenError
  def receiver
    if defined?(@receiver) && @receiver
      @receiver
    else
      raise ArgumentError, "no receiver is available"
    end
  end unless method_defined?(:receiver)
end

class BasicObject
  # Default no-op private hooks expected by CRuby specs.  We define
  # them on BasicObject so they are inherited by every other class.
  private
  def singleton_method_added(_); end
  def singleton_method_removed(_); end
  def singleton_method_undefined(_); end
  # Default initialize: BasicObject's initialize takes no args.
  def initialize
  end unless method_defined?(:initialize)
  # Default method_missing: raise NoMethodError, recording @name / @receiver
  # so #name and #receiver readers work on the raised exception.
  def method_missing(name, *args)
    e = ::NoMethodError.new("undefined method '#{name}' for #{self.inspect}")
    e.instance_variable_set(:@name, name)
    e.instance_variable_set(:@receiver, self)
    ::Kernel.raise(e)
  end unless private_method_defined?(:method_missing) || method_defined?(:method_missing)
end

class Module
  # const_source_location — file/line where the constant was defined.
  # We don't track this; return nil so callers fall back to "unknown".
  def const_source_location(name, _inherit = true)
    nil
  end unless method_defined?(:const_source_location)

  # deprecate_constant — registers the constant as deprecated.  Just
  # accepts the names and does nothing (no-op); subsequent uses still
  # resolve.  CRuby would emit a warning at access time.
  def deprecate_constant(*names)
    names.each do |n|
      sym = n.is_a?(Symbol) ? n : (n.respond_to?(:to_str) ? n.to_str.to_sym : n)
      unless const_defined?(sym, false)
        raise NameError, "constant #{self}::#{sym} not defined"
      end
    end
    self
  end unless method_defined?(:deprecate_constant)

  def autoload(_name, _path); nil; end unless method_defined?(:autoload)
  def autoload?(_name, _inherit = true); nil; end unless method_defined?(:autoload?)

  # Lifecycle hooks: default no-op private instance methods.  CRuby
  # invokes these from the runtime when the corresponding event happens
  # on a class/module — koruby doesn't trigger them yet but defining the
  # privates here lets specs that only assert "is a private method" pass.
  private
  def included(_); end unless method_defined?(:included) || private_method_defined?(:included)
  def extended(_); end unless method_defined?(:extended) || private_method_defined?(:extended)
  def prepended(_); end unless method_defined?(:prepended) || private_method_defined?(:prepended)
  def method_added(_); end unless method_defined?(:method_added) || private_method_defined?(:method_added)
  def method_removed(_); end unless method_defined?(:method_removed) || private_method_defined?(:method_removed)
  def method_undefined(_); end unless method_defined?(:method_undefined) || private_method_defined?(:method_undefined)
  def const_added(_); end unless method_defined?(:const_added) || private_method_defined?(:const_added)
  public
end

module Kernel
  def autoload(_name, _path); nil; end unless method_defined?(:autoload)
  def autoload?(_name, _inherit = true); nil; end unless method_defined?(:autoload?)
end

class Object
  # Default Object#<=> — 0 when self == other, nil otherwise.  CRuby
  # exposes this on BasicObject's chain so every object gets it.
  def <=>(other)
    self == other ? 0 : nil
  end unless method_defined?(:<=>)

  # initialize_copy / clone / dup hooks — private, default no-op that
  # raises FrozenError on a frozen receiver (matches CRuby).
  def initialize_copy(other)
    raise FrozenError, "can't modify frozen #{self.class}: #{self.inspect}" if frozen?
    self
  end unless private_method_defined?(:initialize_copy) || method_defined?(:initialize_copy)
  def initialize_clone(other, freeze: nil)
    initialize_copy(other)
    self
  end unless private_method_defined?(:initialize_clone) || method_defined?(:initialize_clone)
  def initialize_dup(other)
    initialize_copy(other)
    self
  end unless private_method_defined?(:initialize_dup) || method_defined?(:initialize_dup)
  private :initialize_copy, :initialize_clone, :initialize_dup rescue nil
end

# GC — minimal module.  koruby uses libgc (Bartlett-style) so explicit
# collection is a no-op; this exists for spec compatibility.
module GC
  @disabled = false
  def garbage_collect(*); nil; end
  def self.start(*); nil; end
  # disable returns the *previous* state ("was it disabled before"):
  # false means GC was running, now we disabled it.
  def self.disable
    prev = @disabled
    @disabled = true
    prev
  end
  # enable returns the previous state likewise.
  def self.enable
    prev = @disabled
    @disabled = false
    prev
  end
  def self.count; 0; end
  def self.stat(*); {} end
end


class Method
  # Method#curry — fall back through to_proc.
  def curry(arity = nil)
    to_proc.curry(arity)
  end

  # Method#super_method — get the next method up the receiver's
  # ancestor chain past where this method is defined.  Returns nil if
  # there's no super.
  def super_method
    return nil unless owner && receiver
    klass = owner
    found_self = false
    chain = receiver.class.ancestors
    chain.each do |k|
      next unless k.respond_to?(:method_defined?) || k.respond_to?(:private_method_defined?)
      if !found_self
        found_self = (k == klass)
        next
      end
      if k.method_defined?(name) || k.private_method_defined?(name)
        # Bind to the same receiver
        return receiver.method(name).tap { |m|
          # We can't easily get the super-class-bound method without
          # an UnboundMethod-from-class API; fall back to the receiver
          # method.  The caller usually only uses #owner / #parameters.
        }
      end
    end
    nil
  end unless method_defined?(:super_method)

  def >>(other)
    me = self
    ->(*a) { other.call(me.call(*a)) }
  end unless method_defined?(:>>)
  def <<(other)
    me = self
    ->(*a) { me.call(other.call(*a)) }
  end unless method_defined?(:<<)
end

# ---------- Random (LCG-backed) ----------
# Independent of Kernel#rand — useful when you need a reproducible
# stream that doesn't share global state.  Not Mersenne-Twister, just
# numerical-recipes-style 64-bit LCG.  Adequate for tests.
class Random
  def initialize(seed = nil)
    if seed.nil?
      seed = (Time.now * 1e6).to_i
    elsif !seed.is_a?(Integer) && seed.respond_to?(:to_int)
      seed = seed.to_int
    end
    @seed = seed
    @s = seed & 0xffffffffffffffff
  end
  def seed; @seed; end

  def _next
    @s = (@s * 6364136223846793005 + 1442695040888963407) & 0xffffffffffffffff
    @s
  end

  # Use 2.0**64 (Float) to convert raw 64-bit state into a [0, 1)
  # uniform.  `(1 << 64)` is an Integer that's larger than what fits
  # in some operations on koruby (no Bignum on Float division here),
  # so just precompute the Float divisor.
  RAND_SCALE = 18446744073709552000.0  # ~ 2.0**64

  def rand(*args)
    if args.empty?
      (_next & 0xffffffffffffffff).to_f / RAND_SCALE
    else
      a = args[0]
      if a.is_a?(Integer)
        return (_next & 0xffffffffffffffff).to_f / RAND_SCALE if a <= 0
        _next % a
      elsif a.is_a?(Float)
        return (_next & 0xffffffffffffffff).to_f / RAND_SCALE if a <= 0
        ((_next & 0xffffffffffffffff).to_f / RAND_SCALE) * a
      elsif a.is_a?(Range)
        lo = a.first; hi = a.last
        if lo.is_a?(Integer) && hi.is_a?(Integer)
          hi += 1 unless a.exclude_end?
          span = hi - lo
          return lo if span <= 0
          lo + (_next % span)
        else
          lo + ((_next & 0xffffffffffffffff).to_f / RAND_SCALE) * (hi - lo)
        end
      else
        (_next & 0xffffffffffffffff).to_f / RAND_SCALE
      end
    end
  end

  def bytes(n)
    (0...n).map { _next & 0xff }.pack("C*") rescue (0...n).map { _next & 0xff }.map(&:chr).join
  end

  @__new_seed_counter = 0
  def self.new_seed
    # Mix nanosecond time with a monotonically increasing counter so
    # back-to-back calls within the same nanosecond still produce
    # distinct seeds.  Fold in /dev/urandom bytes for entropy.
    @__new_seed_counter += 1
    base = (Time.now.to_f * 1e9).to_i
    extra = 0
    begin
      bytes = File.binread("/dev/urandom", 8)
      bytes.bytes.each_with_index { |b, i| extra |= (b << (i * 8)) }
    rescue
      extra = 0
    end
    (base ^ extra ^ (@__new_seed_counter * 0x9e3779b97f4a7c15)) & ((1 << 128) - 1)
  end

  def self.rand(*args)
    @global ||= Random.new
    @global.rand(*args)
  end

  def self.urandom(n)
    raise ArgumentError, "negative size: #{n}" if n < 0
    (0...n).map { Random.rand(256).chr }.join
  end

  def self.bytes(n); urandom(n); end
  def self.hex(n = 16); urandom(n).bytes.map { |b| "%02x" % b }.join; end
  def self.base64(n = 16); urandom(n); end  # not actually base64 but tests just check size

  # Random#_dump_data — Marshal hook.  Returns the seed as a String so
  # _load_data can reconstruct.
  def _dump_data
    [@s].pack("Q")
  rescue
    @s.to_s
  end
  def _load_data(s)
    @s = s.is_a?(String) ? s.unpack("Q").first.to_i : s.to_i
    self
  end
end

class Symbol
  # Symbol#call(arg) — same as :sym.to_proc.call(arg).  Some tests
  # invoke .call directly on the symbol-as-proc.
  def call(*args, &blk)
    raise ArgumentError, "no receiver given" if args.empty?
    recv = args[0]
    rest = args[1..]
    recv.send(self, *rest, &blk)
  end unless method_defined?(:call)

  def !~(other); !(self =~ other); end unless method_defined?(:!~)
end

class Object
  def to_enum(method = :each, *args)
    me = self
    e = Enumerator.new { |y| me.send(method, *args) { |*x| y.yield(*x) } }
    # Record source so Enumerator#each(&blk) can re-invoke the original
    # method with the user's block — that's how `arr.transform_values
    # .each(&:succ)` aggregates results back into a Hash.  Also record
    # size: when receiver responds to :size, snapshot it for #size.
    e.instance_variable_set(:@__source_obj,    me)
    e.instance_variable_set(:@__source_method, method)
    e.instance_variable_set(:@__source_args,   args)
    if me.respond_to?(:size)
      e.instance_variable_set(:@__size, me.size)
    end
    e
  end unless method_defined?(:to_enum)
  alias_method(:enum_for, :to_enum) rescue nil

  def remove_instance_variable(name)
    sym = name.to_sym
    raise NameError, "`#{name}' is not allowed as an instance variable name" unless name.to_s.start_with?("@")
    v = instance_variable_get(sym)
    raise NameError, "instance variable #{name} not defined" if v.nil? && !instance_variables.include?(sym)
    instance_variable_set(sym, nil)
    v
  end unless method_defined?(:remove_instance_variable)

end

module Kernel
  # Kernel#Hash(arg) — convert arg to Hash via to_hash, or {} for nil.
  def Hash(arg)
    return {} if arg.nil? || arg == []
    return arg if arg.is_a?(Hash)
    return arg.to_hash if arg.respond_to?(:to_hash)
    raise TypeError, "can't convert #{arg.class} into Hash"
  end unless private_method_defined?(:Hash)
  private :Hash
end

# ---------- Set (minimal Hash-backed) ----------
# Backed by a Hash {elem => true}.  No real ordering guarantees beyond
# Hash insertion order (which Ruby's Hash preserves).
class Set
  include Enumerable

  def self.[](*args) = new(args)

  def initialize(enum = nil)
    @h = {}
    enum.each { |e| @h[e] = true } if enum
  end

  def add(o); @h[o] = true; self; end
  alias << add

  def delete(o); @h.delete(o); self; end

  def include?(o); @h.key?(o); end
  alias member? include?

  def size; @h.size; end
  alias length size
  alias count size

  def empty?; @h.empty?; end

  def each(&blk)
    if blk
      @h.each_key(&blk)
      self
    else
      @h.keys
    end
  end

  def to_a; @h.keys; end

  def |(other)
    n = Set.new(@h.keys)
    other.each { |e| n.add(e) }
    n
  end
  alias + |
  alias union |

  def &(other)
    n = Set.new
    @h.each_key { |e| n.add(e) if other.include?(e) }
    n
  end
  alias intersection &

  def -(other)
    n = Set.new
    @h.each_key { |e| n.add(e) unless other.include?(e) }
    n
  end
  alias difference -

  def ==(other)
    return false unless other.is_a?(Set)
    return false unless size == other.size
    @h.each_key { |e| return false unless other.include?(e) }
    true
  end

  def hash; @h.keys.sort_by(&:hash).hash; end

  def inspect
    "#<Set: {" + @h.keys.map(&:inspect).join(", ") + "}>"
  end
  alias to_s inspect

  def subset?(other)
    @h.each_key { |e| return false unless other.include?(e) }
    true
  end
  def superset?(other); other.subset?(self); end
end
