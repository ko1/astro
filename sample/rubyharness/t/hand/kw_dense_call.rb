# o.m(pos, k: v, ...) with every keyword in declared order: the values are already in
# their slots; every other shape (order, missing, extra, opt positional) as CRuby
class C
  def initialize = @base = 100
  def one(x, k: 1) = [x, k]
  def two(x, a:, b: 2) = [x, a, b]
  def locs(x, k: 1)
    t = x + k
    u = defined?(v) ? :v : :none
    [t, u, @base]
  end
  def opt(x, y = 5, k: 1) = [x, y, k]
  def blk(x, k: 1) = block_given? ? yield(x + k) : [:noblk, x, k]
  def ret(x, k: 1) = (return x * k; :never)
  def boom(x, k: 1) = raise(ArgumentError, "boom #{x} #{k}")
  def none(a:, b:) = [a, b]
end
o = C.new
3.times do |i|
  p o.one(i, k: 9), o.two(i, a: 1, b: 3), o.two(i, b: 3, a: 1), o.two(i, a: 7), o.locs(i, k: 2), o.opt(i, k: 3), o.opt(i, 6, k: 3)
  p o.blk(i, k: 2), o.ret(i, k: 4), o.none(a: i, b: i + 1), o.none(b: i, a: i + 1)
end
[-> { o.one(1, z: 2) }, -> { o.two(1, b: 2) }, -> { o.one(1, 2, k: 3) }].each do |pr|
  pr.call
rescue ArgumentError => e
  p e.message
end
begin
  o.boom(1, k: 2)
rescue ArgumentError => e
  p e.message, e.backtrace_locations.map(&:label).first(2)
end
# redefinition seen at the same site
def call_one(o, i) = o.one(i, k: i)
p call_one(o, 1)
class C; def one(x, j: 0, k: 1) = [:redef, x, j, k]; end
p call_one(o, 2)
# deep recursion through the dense path
class R; def down(n, k: 1) = n == 0 ? k : down(n - 1, k: k + 1); end
p R.new.down(1000, k: 0)
