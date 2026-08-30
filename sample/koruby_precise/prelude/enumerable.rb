# Enumerable — defined in terms of #each (the only method a host class must
# provide).  Loaded as part of the koruby_precise prelude.
module Enumerable
  # Gather multi-yields like CRuby: `yield 1, 2` → element [1, 2]; a single value
  # stays as-is.  Collection/search use the gathered element; methods that call a
  # user block forward the raw values so block arity destructures them.  For a
  # single-value each, `a.size == 1` so behaviour is unchanged.
  def __each_el; each { |*a| yield(a.size <= 1 ? a[0] : a) }; end
  # rb_to_int: an Integer as-is, else #to_int, else TypeError (not NoMethodError).
  def __as_int(n)
    return n if n.is_a?(Integer)
    raise TypeError, "no implicit conversion of #{n.class} into Integer" unless n.respond_to?(:to_int)
    n.to_int
  end
  def map(&blk); return __to_enum_sized(:map) unless blk; r = []; each { |*a| r << blk.call(*a) }; r; end
  alias collect map
  # filter methods pass the GATHERED element (a single Array when each yields
  # multiple) to the block; the block auto-splats by its arity. (map/flat_map
  # spread instead: a 1-arg block gets the first value.)
  def select(&blk); return __to_enum_sized(:select) unless blk; r = []; each { |*a| e = a.size <= 1 ? a[0] : a; r << e if blk.call(e) }; r; end
  alias filter select
  alias find_all select
  def reject(&blk); return __to_enum_sized(:reject) unless blk; r = []; each { |*a| e = a.size <= 1 ? a[0] : a; r << e unless blk.call(e) }; r; end
  def grep(pattern, &blk); r = []; each { |*a| e = a.size <= 1 ? a[0] : a; r << (blk ? blk.call(e) : e) if pattern === e }; r; end
  def grep_v(pattern, &blk); r = []; each { |*a| e = a.size <= 1 ? a[0] : a; r << (blk ? blk.call(e) : e) unless pattern === e }; r; end
  # chain(*others) → an Enumerator over self's elements followed by each other's.
  # (koruby's Enumerator is eager, so this materializes; fine for finite sources.)
  def chain(*others) = Enumerator::Chain.new(self, *others)
  def flat_map(&blk); return __to_enum_sized(:flat_map) unless blk; r = []; each { |*a| v = blk.call(*a); if v.is_a?(Array); v.each { |e| r << e }; elsif v.respond_to?(:to_ary); ary = v.to_ary; if ary.is_a?(Array); ary.each { |e| r << e }; elsif ary.nil?; r << v; else; raise TypeError, "can't convert #{v.class} to Array (#{v.class}#to_ary gives #{ary.class})"; end; else; r << v; end }; r; end
  def find(ifnone = nil, &blk); return __to_enum_sized(:find) unless blk; res = nil; found = false; each { |*a| e = a.size <= 1 ? a[0] : a; if blk.call(e); res = e; found = true; break; end }; found ? res : (ifnone ? ifnone.call : nil); end
  alias detect find
  def to_a(*args)
    r = []
    if args.empty?   # __each_el indirection: a literal block handed straight to a
                     # callee with an &b param is lost when THIS method runs under a
                     # to_enum-forwarded (CPROC) block — see todo.md
      __each_el { |e| r << e }
    else
      each(*args) { |*a| r << (a.size <= 1 ? a[0] : a) }   # extra args go to #each (CRuby)
    end
    r
  end
  def lazy; to_a.lazy; end   # finite-source lazy (an infinite custom #each would need a true lazy driver)
  def to_h(*args, &blk); h = {}; each(*args) { |*a| pair = blk ? blk.call(*a) : (a.size <= 1 ? a[0] : a); pair = pair.to_ary if !pair.is_a?(Array) && pair.respond_to?(:to_ary); raise TypeError, "wrong element type #{pair.class} (expected array)" unless pair.is_a?(Array); raise ArgumentError, "element has wrong array length (expected 2, was #{pair.size})" unless pair.size == 2; h[pair[0]] = pair[1] }; h; end
  alias entries to_a
  def count(*args, &blk); raise ArgumentError, "wrong number of arguments (given #{args.size}, expected 0..1)" if args.size > 1; n = 0; if args.size > 0; warn "given block not used" if blk; item = args[0]; __each_el { |x| n += 1 if x == item }; elsif blk; each { |*a| n += 1 if blk.call(*a) }; else; each { |*a| n += 1 }; end; n; end
  def include?(v); __each_el { |e| return true if e == v }; false; end
  alias member? include?
  def first(*args)
    if args.empty?                         # no argument → the first element (an explicit nil is NOT "no arg")
      __each_el { |e| return e }
      return nil
    end
    raise ArgumentError, "wrong number of arguments (given #{args.size}, expected 0..1)" if args.size > 1
    n = args[0]
    unless n.is_a?(Integer)                # coerce via #to_int; a non-numeric arg (incl. nil) is a TypeError
      raise TypeError, "no implicit conversion of #{n.class} into Integer" unless n.respond_to?(:to_int)
      n = n.to_int
    end
    raise ArgumentError, "negative array size" if n < 0
    raise RangeError, "bignum too big to convert into `long'" if n > 0x7fffffffffffffff
    r = []; c = 0
    __each_el { |e| break if c >= n; r << e; c += 1 }
    r
  end
  # reduce/inject: (sym) | (init, sym) | { block } | (init) { block }.
  def reduce(*args)
    if block_given?
      if args.size >= 1 then acc = args[0]; f = false else acc = nil; f = true end
      __each_el { |x| if f then acc = x; f = false else acc = yield(acc, x) end }
      return acc
    end
    if args.size >= 2 then acc = args[0]; op = args[1]; f = false else op = args[0]; acc = nil; f = true end
    # the operator/method-name arg is a Symbol/String, or #to_str-coercible; else TypeError.
    unless op.is_a?(Symbol) || op.is_a?(String)
      raise TypeError, "#{op.inspect} is not a symbol nor a string" unless op.respond_to?(:to_str)
      op = op.to_str
    end
    __each_el { |x| if f then acc = x; f = false else acc = acc.send(op, x) end }
    acc
  end
  alias inject reduce
  # Kahan-Babuška compensated summation once the running total and the addend are
  # both Float (matches CRuby's precise Float sum); exact for Integer/Rational.
  def sum(init = 0, &blk)
    s = init; comp = 0.0
    each do |*a|
      x = blk ? blk.call(a.size <= 1 ? a[0] : a) : (a.size <= 1 ? a[0] : a)
      if s.is_a?(Float) && x.is_a?(Float)
        t = s + x
        comp += (s.abs >= x.abs) ? ((s - t) + x) : ((x - t) + s)
        s = t
      else
        s = s + x
      end
    end
    s.is_a?(Float) ? s + comp : s
  end
  # min/max: (), (n), { cmp }, (n) { cmp }.  n → the n smallest/largest as an Array.
  # Running min/max (not sort-based): honours a degenerate comparator block that
  # keeps the first element (CRuby semantics). The n form returns the n smallest/largest.
  def min(n = nil, &blk); return (blk ? to_a.sort(&blk) : to_a.sort).first(n) if n; r = nil; f = true; __each_el { |x| if f; r = x; f = false; else; c = blk ? blk.call(x, r) : (x <=> r); raise ArgumentError, "comparison of #{x.class} with #{r.class} failed" if c.nil?; r = x if c < 0; end }; r; end
  def max(n = nil, &blk); return (blk ? to_a.sort(&blk) : to_a.sort).last(n).reverse if n; r = nil; f = true; __each_el { |x| if f; r = x; f = false; else; c = blk ? blk.call(x, r) : (x <=> r); raise ArgumentError, "comparison of #{x.class} with #{r.class} failed" if c.nil?; r = x if c > 0; end }; r; end
  def min_by(n = nil); return __to_enum_sized(:min_by) unless block_given?; s = sort_by { |x| yield(x) }; n ? s.first(n) : s.first; end
  def max_by(n = nil); return __to_enum_sized(:max_by) unless block_given?; s = sort_by { |x| yield(x) }; n ? s.last(n).reverse : s.last; end
  def sort(&blk); to_a.sort(&blk); end
  def sort_by; return __to_enum_sized(:sort_by) unless block_given?; a = []; __each_el { |x| a << x }; a.sort_by { |x| yield(x) }; end
  # all?/any?/none?/one? accept an optional pattern (uses pattern === x), else a block, else truthiness.
  def all?(*a, &blk); raise ArgumentError, "wrong number of arguments (given #{a.size}, expected 0..1)" if a.size > 1; if a.size > 0; warn "given block not used" if blk; pt = a[0]; __each_el { |x| return false unless pt === x }; elsif blk; ok = true; each { |*ar| unless blk.call(*ar); ok = false; break; end }; return ok; else; __each_el { |x| return false unless x }; end; true; end
  def any?(*a, &blk); raise ArgumentError, "wrong number of arguments (given #{a.size}, expected 0..1)" if a.size > 1; if a.size > 0; warn "given block not used" if blk; pt = a[0]; __each_el { |x| return true if pt === x }; elsif blk; hit = false; each { |*ar| if blk.call(*ar); hit = true; break; end }; return hit; else; __each_el { |x| return true if x }; end; false; end
  def none?(*a, &blk); raise ArgumentError, "wrong number of arguments (given #{a.size}, expected 0..1)" if a.size > 1; if a.size > 0; warn "given block not used" if blk; pt = a[0]; __each_el { |x| return false if pt === x }; elsif blk; hit = false; each { |*ar| if blk.call(*ar); hit = true; break; end }; return !hit; else; __each_el { |x| return false if x }; end; true; end
  def one?(*a, &blk); raise ArgumentError, "wrong number of arguments (given #{a.size}, expected 0..1)" if a.size > 1; n = 0; if a.size > 0; warn "given block not used" if blk; pt = a[0]; __each_el { |x| (n += 1; break if n > 1) if pt === x }; elsif blk; each { |*ar| (n += 1; break if n > 1) if blk.call(*ar) }; else; __each_el { |x| (n += 1; break if n > 1) if x }; end; n == 1; end
  def cycle(n = nil, &blk)                                          # delegate to Array#cycle (break handled there)
    # no block → Enumerator (each not called until iterated); its size is the
    # receiver's times n, or Infinity when cycling forever
    unless blk
      return to_enum(:cycle, n) { s = respond_to?(:size) ? size : nil
                                  next nil if s.nil?
                                  next 0 if s == 0                 # nothing to cycle → 0, even forever
                                  n.nil? ? Float::INFINITY : s * (n < 0 ? 0 : n) }
    end
    ni = nil
    unless n.nil?
      ni = if n.is_a?(Integer) then n
           elsif n.respond_to?(:to_int) then n.to_int
           else raise TypeError, "no implicit conversion of #{n.class} into Integer"
           end
      return nil if ni <= 0                                        # non-positive count → nil, without iterating
    end
    # the first pass yields straight off #each (CRuby only buffers as it goes;
    # a `break` in the block must stop before the source is drained)
    buf = []
    __each_el { |x| buf << x; yield x }
    return nil if buf.empty?
    if ni.nil?
      loop { buf.each { |x| yield x } }
    else
      (ni - 1).times { buf.each { |x| yield x } }
    end
    nil
  end
  def each_with_object(o); return __to_enum_sized(:each_with_object, o) unless block_given?; __each_el { |x| yield x, o }; o; end
  def each_with_index(*args)
    return __to_enum_sized(:each_with_index, *args) unless block_given?
    i = 0
    if args.empty?
      __each_el { |x| yield x, i; i += 1 }
    else
      each(*args) { |*a| yield((a.size <= 1 ? a[0] : a), i); i += 1 }
    end
    self
  end
  def partition(&blk); return __to_enum_sized(:partition) unless blk; a = []; b = []; each { |*ar| e = ar.size <= 1 ? ar[0] : ar; if blk.call(e); a << e; else; b << e; end }; [a, b]; end
  def group_by; return __to_enum_sized(:group_by) unless block_given?; h = {}; __each_el { |x| k = yield(x); h[k] = [] unless h.key?(k); h[k] << x }; h; end
  def tally(h = {}); h = h.to_hash if !h.is_a?(Hash) && h.respond_to?(:to_hash); __each_el { |x| h[x] = (h.key?(x) ? h[x] : 0) + 1 }; h; end
  # CRuby yields [key, elements] pairs from an Enumerator whose #size is nil.
  def chunk
    return to_enum(:chunk) unless block_given?
    r = []; lastk = nil; f = true
    __each_el { |x|
      k = yield(x)
      if k.nil? || k == :_separator; f = true; lastk = nil; next; end
      if k.is_a?(Symbol) && k.to_s.start_with?("_") && k != :_alone
        raise RuntimeError, "symbols beginning with an underscore are reserved"
      end
      if k == :_alone
        r << [k, [x]]; f = true; lastk = nil
      elsif f || k != lastk
        r << [k, [x]]; f = false; lastk = k
      else
        r.last[1] << x; lastk = k
      end
    }
    Enumerator.new { |y| r.each { |ch| y << ch } }
  end
  def chunk_while; raise ArgumentError, "tried to create Proc object without a block" unless block_given?; r = []; cur = nil; f = true; prev = nil; __each_el { |x| if f; cur = [x]; f = false; elsif yield(prev, x); cur << x; else; r << cur; cur = [x]; end; prev = x }; r << cur unless cur.nil?; r; end
  def slice_when; raise ArgumentError, "tried to create Proc object without a block" unless block_given?; r = []; cur = nil; f = true; prev = nil; __each_el { |x| if f; cur = [x]; f = false; elsif yield(prev, x); r << cur; cur = [x]; else; cur << x; end; prev = x }; r << cur unless cur.nil?; r; end
  def slice_before(*pat, &b)
    raise ArgumentError, "wrong number of arguments (given #{pat.size}, expected 1)" if b ? !pat.empty? : pat.size != 1
    Enumerator.new do |y|
      cur = nil
      __each_el { |x| t = b ? b.call(x) : (pat[0] === x); if t && cur; y << cur; cur = [x]; elsif cur; cur << x; else; cur = [x]; end }
      y << cur if cur
    end
  end
  def slice_after(*pat, &b)
    raise ArgumentError, "wrong number of arguments (given #{pat.size}, expected 1)" if b ? !pat.empty? : pat.size != 1
    Enumerator.new do |y|
      cur = []
      __each_el { |x| cur << x; t = b ? b.call(x) : (pat[0] === x); if t; y << cur; cur = []; end }
      y << cur unless cur.empty?
    end
  end
  def take(n); n = __as_int(n); raise ArgumentError, "attempt to take negative size" if n < 0; r = []; __each_el { |x| break if r.size >= n; r << x }; r; end
  def drop(n); n = __as_int(n); raise ArgumentError, "attempt to drop negative size" if n < 0; r = []; i = 0; __each_el { |x| r << x if i >= n; i += 1 }; r; end
  def take_while(&blk); return __to_enum_sized(:take_while) unless blk; r = []; each { |*ar| e = ar.size <= 1 ? ar[0] : ar; break unless blk.call(*ar); r << e }; r; end
  def drop_while(&blk); return __to_enum_sized(:drop_while) unless blk; r = []; dropping = true; each { |*ar| e = ar.size <= 1 ? ar[0] : ar; dropping = false if dropping && !blk.call(e); r << e unless dropping }; r; end
  def minmax(&blk); [min(&blk), max(&blk)]; end   # honor an optional comparator block
  def minmax_by; return __to_enum_sized(:minmax_by) unless block_given?; [min_by { |x| yield(x) }, max_by { |x| yield(x) }]; end
  def find_index(*v, &blk); return __to_enum_sized(:find_index) if !blk && v.empty?; i = 0; if blk && v.empty?; idx = nil; each { |*ar| if blk.call(*ar); idx = i; break; end; i += 1 }; return idx; else; warn "given block not used" if blk; t = v[0]; __each_el { |x| return i if x == t; i += 1 }; end; nil; end
  def each_slice(n, &blk); n = __as_int(n); raise ArgumentError, "invalid slice size" unless n > 0; unless blk; this = self; sz = (respond_to?(:size) && (z = size)) ? (z + n - 1) / n : nil; return Enumerator.new(sz) { |y| this.each_slice(n) { |s| y << s } }; end; s = []; __each_el { |x| s << x; if s.size == n; yield s; s = []; end }; yield s unless s.empty?; self; end
  def each_cons(n, &blk); n = __as_int(n); raise ArgumentError, "invalid size" unless n > 0; unless blk; this = self; sz = (respond_to?(:size) && (z = size)) ? (z >= n ? z - n + 1 : 0) : nil; return Enumerator.new(sz) { |y| this.each_cons(n) { |c| y << c } }; end; buf = []; __each_el { |x| buf << x; if buf.size == n; yield buf.dup; buf.shift; end }; self; end
  def zip(*others)
    os = others.map do |o|                          # to_ary, else drive #each via to_enum(:each), else TypeError
      if o.respond_to?(:to_ary) then o.to_ary
      elsif o.respond_to?(:each) then o.to_enum(:each).to_a
      else raise TypeError, "wrong argument type #{o.class} (must respond to :each)"
      end
    end
    r = []; i = 0; __each_el { |x| row = [x]; os.each { |o| row << o[i] }; r << row; i += 1 }
    if block_given? then r.each { |row| yield row }; nil else r end
  end
  def filter_map(&blk); return __to_enum_sized(:filter_map) unless blk; r = []; each { |*ar| v = blk.call(*ar); r << v if v }; r; end
  alias collect_concat flat_map
  def reverse_each; return __to_enum_sized(:reverse_each) unless block_given?; to_a.reverse.each { |x| yield x }; self; end
  def uniq; seen = {}; r = []; __each_el { |x| k = block_given? ? yield(x) : x; unless seen.key?(k); seen[k] = true; r << x; end }; r; end
  def each_entry(*args)
    return __to_enum_sized(:each_entry, *args) unless block_given?
    if args.empty?
      __each_el { |x| yield x }
    else
      each(*args) { |*a| yield(a.size <= 1 ? a[0] : a) }
    end
    self
  end
  def compact; r = []; __each_el { |x| r << x unless x.nil? }; r; end
end

# MatchData pattern-matching support.
class MatchData
  def deconstruct; captures; end
end
