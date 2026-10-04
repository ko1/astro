# `super(a, b)` with plain positional args (staged like a call, no Array) keeps forwarding the block and every super shape
class P
  def m0; [:p0, block_given? ? yield : nil]; end
  def m1(x); [:p1, x, block_given? ? yield(x) : nil]; end
  def m2(x, y = :dy); [:p2, x, y]; end
  def m3(x, *r); [:p3, x, r]; end
  def kw(x, k: 0); [:pk, x, k]; end
  def opt(h = {}); [:po, h]; end
  def rec(n); n; end
  def mm(x); [:pmm, x]; end
end
class C < P
  def m0; super(); end
  def m1(x); super(x + 1); end
  def m2(x); [super(x), super(x, :y)]; end
  def m3(x); super(x, 1, 2); end
  def kw(x); super(x); end
  def opt(h); super(h); end
  def rec(n); n == 0 ? super(n) : rec(n - 1); end
  def mm(x); super(x.then { |v| v * 2 }); end
end
c = C.new
p c.m0, c.m0 { :blk }, c.m1(1), c.m1(1) { |v| v * 10 }, c.m2(5), c.m3(7), c.kw(3), c.opt({ a: 1 }), c.rec(3), c.mm(4)
class D < C
  def m1(x)
    [1].map { |v| super(x) }   # inside a block: not the staged form
  end
  define_method(:m0) { super() }
end
d = D.new
p d.m1(2), d.m0 { :dm }
module Pre; def m1(x); [:pre, super(x)]; end; end
class E < P; prepend Pre; def m1(x); [:e, super(x * 3)]; end; end
p E.new.m1(1)
class H < Hash
  def initialize(d); super(d); @t = 1; end
  def fetch(k); super(k, :none); end
  def store(k, v); super(k, v * 2); end
end
h = H.new(:dflt); h.store(:a, 5)
p h[:zz], h.fetch(:a), h.fetch(:q), h
class S < String; def initialize(s); super(s.upcase); end; end
p S.new("abc")
class K
  def self.make(x); [:make, x]; end
end
class K2 < K
  def self.make(x); super(x + 1); end
  def self.new(*a); super(*a); end
end
p K2.make(1), K2.new.class
class MM
  def method_missing(n, *a) = n == :zz ? [:mm, a] : super
  def respond_to_missing?(n, p = false) = n == :zz || super
end
class MM2 < MM; def zz(a, b); super(a, b); end; end
p MM2.new.zz(1, 2)
class Raise < P
  def m1(x); super(Integer("nope")); rescue ArgumentError => e; [:rescued, e.class]; end
end
p Raise.new.m1(1)
class G < P
  def m2(x); super(Array.new(100) { "s" * 30 }.size, (GC.start; :after_gc)); end
end
3.times { p G.new.m2(0) }
class Deep < P
  def m1(x); x > 0 ? Deep.new.m1(x - 1) + [super(x)] : [super(x)]; end
end
p Deep.new.m1(2).size
class NoParent; def m1(x); super(x); end; end
begin
  NoParent.new.m1(1)
rescue NoMethodError => e
  p e.class, e.message[0, 40]
end
