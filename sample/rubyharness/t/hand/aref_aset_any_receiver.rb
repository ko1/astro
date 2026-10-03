# recv[i] / recv[i] = v on receivers the node's type fast path does not take: they go through
# the node's inline cache; redefinitions, method_missing, refinements and errors as CRuby
class Headers < Hash
  def [](k) = super(k.to_s.downcase)
  def []=(k, v)
    super(k.to_s.downcase, v)
  end
end
module Registry
  @h = {}
  def self.[](k) = @h[k]
  def self.[]=(k, v)
    @h[k] = v
  end
end
S = Struct.new(:a, :b)
class MM
  def method_missing(n, *a) = n == :[] ? [:mm, *a] : super
  def respond_to_missing?(n, p = false) = n == :[] || super
end
class Boom; def [](k) = raise(KeyError, "boom #{k}"); end
def read(o, k) = o[k]
def write(o, k, v) = (o[k] = v)
h = Headers.new
3.times do |i|
  write(h, "Content-Type", "t#{i}")
  write(Registry, :k, i)
  m = /(?<x>\d+)-(\d+)/.match("12-#{i}")
  st = S.new(i, i * 2)
  p read(h, "CONTENT-TYPE"), read(Registry, :k), read(m, :x), read(m, 2), read(st, :b), read(st, 0),
    read(->(x) { x * 10 }, i), read(MM.new, i), read({ a: i }, :a), read([i, 9], 1), read("hey", i)
end
begin
  read(Boom.new, 7)
rescue KeyError => e
  p e.message, e.backtrace_locations.map(&:label).first(2)
end
# redefinition at the same sites
class Headers; def [](k) = :redefined; end
p read(h, "x")
module Registry; def self.[](k) = [:redef, k]; end
p read(Registry, :q)
# the value of an assignment expression is the assigned value
p write(Registry, :z, 42), write(h, "a", 1)
# a refinement of [] at a refined site
module RH; refine(Headers) { def [](k) = :refined }; end
def unrefined_read(o) = o["x"]
using RH
def refined_read(o) = o["x"]
p [unrefined_read(h), refined_read(h)]
