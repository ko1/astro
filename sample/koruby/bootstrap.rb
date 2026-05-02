# Bootstrap — Ruby methods loaded before any user program.
#
# Defines methods on built-in modules/classes that are easier to write
# in Ruby than in C.  Any class included into Enumerable picks up all
# the iterator helpers below by virtue of needing only `each`.

class Enumerable
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
end

# Array, Hash, Range, String all support `each`, so wire up Enumerable.
# `include` below adds only methods that the target class doesn't
# already define (cfuncs win), so Array#map etc. stay fast.
class Array
  include Enumerable
end

class Hash
  include Enumerable
end

class Range
  include Enumerable
end

# Numeric / String have <=> so they pick up Comparable's between?/clamp.
# Integer / Float / String / Symbol include Comparable directly so
# their methods table holds the snapshot.  Numeric also includes
# Comparable for the benefit of `Integer < Numeric < Comparable`
# ancestor checks and user `class Foo < Numeric` getting between? etc.
class Numeric
  include Comparable
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

  def digits(base = 10)
    raise ArgumentError, "negative number" if self < 0
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
end

class String
  include Comparable
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
  attr_reader :numerator, :denominator
  alias num numerator
  alias den denominator

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
      Rational.new(@numerator * other.denominator + other.numerator * @denominator, @denominator * other.denominator)
    when Integer
      Rational.new(@numerator + other * @denominator, @denominator)
    when Float
      to_f + other
    end
  end

  def -(other)
    case other
    when Rational
      Rational.new(@numerator * other.denominator - other.numerator * @denominator, @denominator * other.denominator)
    when Integer
      Rational.new(@numerator - other * @denominator, @denominator)
    when Float
      to_f - other
    end
  end

  def *(other)
    case other
    when Rational
      Rational.new(@numerator * other.numerator, @denominator * other.denominator)
    when Integer
      Rational.new(@numerator * other, @denominator)
    when Float
      to_f * other
    end
  end

  def /(other)
    case other
    when Rational
      Rational.new(@numerator * other.denominator, @denominator * other.numerator)
    when Integer
      Rational.new(@numerator, @denominator * other)
    when Float
      to_f / other
    end
  end

  def -@; Rational.new(-@numerator, @denominator); end

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
end

def Rational(n, d = 1)
  Rational.new(n, d)
end

# Integer/Float arithmetic with Rational/Complex — handled in C-side
# int_plus / int_minus / int_mul / int_div by checking the operand's
# class and delegating to the Rational/Complex class.  Implemented in
# builtins.c so the fast path stays a cfunc.

# Complex — Cartesian (real, imag) representation.
class Complex
  attr_reader :real, :imaginary
  alias imag imaginary

  def initialize(r, i = 0)
    @real = r
    @imaginary = i
  end

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

  def ==(other)
    if other.is_a?(Complex)
      @real == other.real && @imaginary == other.imag
    else
      @imaginary == 0 && @real == other
    end
  end

  def abs; Math.sqrt(@real * @real + @imaginary * @imaginary); end
  def conjugate; Complex.new(@real, -@imaginary); end
  alias conj conjugate

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
class Enumerable
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
  def transform_values(&blk)
    h = {}
    each_pair { |k, v| h[k] = blk.call(v) }
    h
  end

  def transform_keys(&blk)
    h = {}
    each_pair { |k, v| h[blk.call(k)] = v }
    h
  end

  def to_h
    self
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

  def sort
    to_a.sort
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

  def lstrip
    s = self.dup
    s.sub(/\A\s+/, "")
    while s.size > 0 && (s[0] == " " || s[0] == "\t" || s[0] == "\n" || s[0] == "\r")
      s = s[1..-1]
    end
    s
  end

  def rstrip
    s = self.dup
    while s.size > 0 && (s[-1] == " " || s[-1] == "\t" || s[-1] == "\n" || s[-1] == "\r")
      s = s[0..-2]
    end
    s
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

class Proc
  # Curry: each call accumulates args until enough; then invokes self.
  # Args can come singly (`c[1][2][3]`) or multiply (`c[1, 2][3]`).
  def curry(arity = nil)
    n = arity || self.arity
    n = -n if n < 0
    me = self
    accum = nil
    accum = ->(collected) {
      ->(*more) {
        all = collected + more
        if all.size >= n
          me.call(*all)
        else
          accum.call(all)
        end
      }
    }
    accum.call([])
  end
end

class Method
  # Method#curry — fall back through to_proc.
  def curry(arity = nil)
    to_proc.curry(arity)
  end
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
