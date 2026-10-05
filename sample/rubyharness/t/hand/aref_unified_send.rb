# recv[i] / recv[i] = v for every kind of receiver, now dispatched as the named send `recv.[](i)` would be:
# builtins and their subclasses, Struct, Thread, MatchData, Method, user objects (Ruby / method_missing / refined),
# the assignment's value when the callee reassigns its parameter, raises with their backtraces
class HSub < Hash; def [](k) = super || :dflt; end
class ASub < Array
  def []=(i, v)
    super(i, v * 10)
  end
end
class Bag
  def initialize = @h = {}
  def [](k) = @h[k]
  def []=(k, v)
    @h[k] = v
    v = :clobbered
    :ret_ignored
  end
end
class Ghost; def method_missing(n, *a) = [n, *a]; def respond_to_missing?(*) = true; end
S = Struct.new(:a, :b)
module Ref; refine(Bag) { def [](k) = [:refined, k] }; end
bag = Bag.new
3.times do |i|
  GC.start if i == 1
  s = "hello"; h = Hash.new { |hh, k| hh[k] = k.to_s * 2 }; hs = HSub[a: 1]; as = ASub[1, 2]
  p s[1], s[1..2], s[/l+/], s["ell"], [1, 2, 3][1..], { a: 1 }[:a], h[:x], h, hs[:a], hs[:zz]
  p (as[0] = i), as, (bag[:k] = i), bag[:k], (bag[:z] ||= 5), bag[:z]
  st = S.new(1, 2); st[:a] = i; st[1] = :b
  p st[0], st[:b], st["a"], Thread.current[:tk] = i, Thread.current[:tk], /(\w)(\w)/.match("xy")[2]
  p 1.method(:+)[i], 0b1010[1], 1.5.then { |f| [f].first }, Ghost.new[1, 2], Ghost.new[:q]
  str = +"abc"; str[1] = "Z"; p str, (str[0, 2] = "q"), str
end
using Ref
p bag[:k], (bag[:n] = 1)
begin
  {}.freeze[:a] = 1
rescue => e
  p e.class
end
class Boom
  def [](k) = raise(IndexError, "bad #{k}")
  def []=(k, v)
    raise(KeyError, "no set")
  end
end
bm = Boom.new
begin
  bm[3]
rescue IndexError => e
  p e.message, e.backtrace_locations[0, 2].map(&:label)
end
begin
  bm[3] = 4
rescue KeyError => e
  p e.message, e.backtrace_locations[0, 2].map(&:label)
end
x = [Bag.new, { k: 9 }, [7, 8], "kq", S.new(:k, 1), HSub.new]
4.times { p x.map { |o| o[o.is_a?(Array) || o.is_a?(String) || o.is_a?(Struct) ? 0 : :k] } }
