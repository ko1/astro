# korb_send_cached's small entry: a keyword method called without keywords, and a Ruby
# method added to a builtin class, through cached sites (plus what must still go the full way)
class String; def to_liquid = self; def twice(n = 2) = self * n; end
class Hash; def to_liquid = self; end
class Integer; def to_liquid = self; end
class Array; def to_liquid = self; end
class Float; def half = self / 2; end
class Ctx
  def find(key, raise_on: true) = [key, raise_on]
  def lookup(obj, key, raise_on: true) = [obj, key, raise_on]
  def opt(a, b = 1, k: 2) = [a, b, k]
  def blk(a, k: 1) = block_given? ? yield(a, k) : [:noblk, a, k]
end
c = Ctx.new
vals = ["s", { a: 1 }, 3, [1], 2.5]
3.times do |i|
  p c.find(i), c.lookup(i, :x), c.opt(i), c.opt(i, 5), c.blk(i), c.blk(i) { |a, k| a + k }
  p vals.map { (_1.to_liquid rescue :nm) }, "ab".twice, "ab".twice(3), 2.5.half
end
# a kwargs-marked Hash as the last argument goes the general way
h = { raise_on: false }
p c.find(1, **h), (c.find(1, h) rescue $!.class)
# redefinitions seen through the cached sites
class Ctx; def find(key, raise_on: :redef) = [:new, key, raise_on]; end
p c.find(9)
class String; def to_liquid = :string_redef; end
p "x".to_liquid
# a singleton / extended builtin goes the full way
s = +"single"
def s.to_liquid = :singleton
p s.to_liquid, "plain".to_liquid
module Ext; def to_liquid = :ext; end
t = +"ext"
t.extend(Ext)
p t.to_liquid
# refinement at a site
module RS; refine(String) { def to_liquid = :refined }; end
def unrefined(x) = x.to_liquid
using RS
def refined(x) = x.to_liquid
p [unrefined("a"), refined("a")]
