# korb_send_cached handles more cache hits itself instead of korb_send_cached_full: class methods with optional /
# keyword params, `super` from a prepended module (ERB::Util-style), define_method'd ones; instance hits on
# private / protected methods (self.m allowed, outside calls refused), visibility changed after caching, refinements
module Escaper
  def esc(s, quote = true) = "esc(#{super(s)},#{quote})"
end
module Util
  def self.esc(s) = s.to_s.upcase
  def self.opt(a, b = 2, *r) = [a, b, r]
  def self.kw(a, k: 1) = [a, k]
  define_singleton_method(:dm) { |x = 5| x * 2 }
  singleton_class.prepend(Escaper)
end
class Opts < Hash
  alias _get []
  private :_get
  def get(k) = self._get(k)
  def get2(k) = _get(k)
  def peer(o) = o.prot
  protected def prot = :protected_ok
end
o = Opts.new.merge!(a: 1)
4.times do |i|
  GC.start if i == 2
  p Util.esc("ab"), Util.esc(:cd, false), Util.opt(1), Util.opt(1, 3, 4), Util.kw(i), Util.kw(i, k: 9), Util.dm, Util.dm(i)
  p o.get(:a), o.get2(:a), o.peer(Opts.new)
  begin
    o._get(:a)
  rescue NoMethodError => e
    p e.message[/private method '_get'/]
  end
  begin
    o.prot
  rescue NoMethodError => e
    p e.message[/protected method 'prot'/]
  end
end
class Opts; public :_get; end
p o._get(:a)
class Opts; private :_get; end
p((o._get(:a) rescue :private_again))
module Util; def self.opt(a, b = 7) = [:redefined, a, b]; end
p Util.opt(1)
module Refine; refine(Util.singleton_class) { def kw(a, k: 1) = [:refined, a, k] }; end
using Refine
p Util.kw(3)
