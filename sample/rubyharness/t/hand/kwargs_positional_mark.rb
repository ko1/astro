# The "these were keywords" mark belongs to the CALL, not to the Hash.  A Hash
# written as bare `k => v` and bound to a POSITIONAL parameter is a plain Hash
# from there on — simplecov's `thresholds_for(path, /re/ => {...})` passed its
# `overrides` on to a method with a `baseline:` keyword and got
# "unknown keyword: Regexp".
def inner(a, over = {}, baseline: nil)
  [a, over, baseline]
end
def outer(name, overrides) = inner(name, overrides)
p outer("x", /re/ => {line: 90})
p outer("x", "s" => 1)
p outer("x", sym: 1)

def outer2(name, overrides) = inner(name, overrides, baseline: :b)
p outer2("x", /re/ => 1)

# a block that binds it positionally drops the mark too
p ->(n, h) { inner(n, h) }.call("y", k: 1)

# ...and a *rest without ruby2_keywords does the same (CRuby 3 semantics)
def sink(*a, **k) = [a, k]
class C
  def sink(*a, **k) = [a, k]
  def plain(*a)  = sink(*a)      # kwargs become a positional Hash
  def kw(*a, **k) = sink(*a, **k)
  def dots(...)  = sink(...)
  def r2k(*a)    = sink(*a)
  ruby2_keywords :r2k
  def collect(*a) = a
  ruby2_keywords :collect
  def aliased(*a) = sink(*a)
  alias_method :aliased2, :aliased
  ruby2_keywords :aliased        # flags the alias made BEFORE this call too
end
c = C.new
p c.plain(1, x: 2)
p c.kw(1, x: 2)
p c.dots(1, x: 2)
p c.r2k(1, x: 2)
p c.aliased2(1, x: 2)

h = {y: 3}
p c.plain(1, **h)
p c.plain(1, h)

pr = proc { |*a| sink(*a) }
p pr.call(1, x: 2)
pr2 = proc { |*a| sink(*a) }
pr2.ruby2_keywords
p pr2.call(1, x: 2)
pr3 = proc { |*a| sink(*a) }
pr4 = pr3.dup
pr3.ruby2_keywords                 # the flag is on the ISEQ, so pr4 has it too
p pr4.call(1, x: 2)

# the persistent mark is what ruby2_keywords_hash? reports, and a Hash passed
# DIRECTLY (not splatted) as a positional argument keeps it
marked = c.collect(1, a: 1).last
p Hash.ruby2_keywords_hash?(marked)
def single(arg) = arg
p Hash.ruby2_keywords_hash?(single(marked))
p single(marked).equal?(marked)

require "delegate"
class T; def m(*a, **k) = [a, k]; end
class D < SimpleDelegator; end
p D.new(T.new).m(1, x: 2)

# unknown keywords are reported together, and a non-Symbol key by value
def kwonly(kw:) = kw
begin; kwonly(kw: 1, a: 1, b: 2, c: 3); rescue ArgumentError => e; p e.message; end
begin; kwonly(kw: 1, true => false); rescue ArgumentError => e; p e.message; end
begin; kwonly(kw: 1, 7 => 0); rescue ArgumentError => e; p e.message; end
begin; kwonly(kw: 1, "s" => 0); rescue ArgumentError => e; p e.message; end
