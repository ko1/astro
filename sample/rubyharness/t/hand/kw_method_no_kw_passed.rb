# a keyword method called with no keywords: the defaults bind in the method's scope
class C
  def initialize = @base = 10
  def find(key, raise_on: true) = [key, raise_on]
  def two(a, b = a + 1, k: b * 2, j: @base) = [a, b, k, j]
  def req(a, k:) = [a, k]
  def blk(a, k: 1) = block_given? ? yield(a + k) : [:noblk, a, k]
  def h(a, k: 1) = [a, k]
  def redef(a, k: (C.class_eval { def redef(*) = :new }; :dflt)) = [a, k]
  def bt(a, k: raise(ArgumentError, "dflt #{a}")) = a
end
o = C.new
3.times do |i|
  p o.find(i), o.find(i, raise_on: false), o.two(i), o.two(i, 5), o.two(i, 5, k: 0), o.blk(i), o.blk(i) { _1 * 100 }
end
# missing required keyword, too many positionals
[-> { o.req(1) }, -> { o.find(1, 2) }, -> { o.find }].each do |pr|
  pr.call
rescue ArgumentError => e
  p e.message
end
# a trailing plain Hash is a positional argument (Ruby 3), a kwargs splat is keywords
hh = { k: 9 }
p o.h(1, **hh)
begin
  o.h(1, hh)
rescue ArgumentError => e
  p e.message
end
p o.redef(1), o.redef(2)
begin
  o.bt(3)
rescue ArgumentError => e
  p e.message
end
# Klass.new → initialize with keyword defaults
class D
  def initialize(a, k: :kd) = (@v = [a, k])
  attr_reader :v
end
p D.new(1).v, D.new(2, k: 3).v
# super into a keyword method without keywords
class E < C; def find(key) = super; end
p E.new.find(:x)
# a def inside instance_eval while a keyword-default method runs
class F; def m(a, k: 1) = (def made_by_kw = :made; a + k); end
f = F.new
f.instance_eval { m(1) }
p f.respond_to?(:made_by_kw), F.instance_methods(false).sort
