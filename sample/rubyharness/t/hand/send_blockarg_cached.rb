# recv.m(args, &blk) through the site's inline cache: Ruby and C callees, &:sym / &method / &nil, break/return, privacy, misses
class Box
  def initialize = @h = { a: 1 }
  def get(k, &b) = @h.fetch(k, &b)
  def each(&b) = [1, 2].each(&b)
  def yielder = block_given? ? yield(10) : :noblk
  def via(&b) = yielder(&b)
  def map_sym(a) = a.map(&:to_s)
  def meth(a) = a.map(&method(:twice))
  def twice(x) = x * 2
  def nilblk = yielder(&nil)
  def priv_call(&b) = self.hidden(&b)
  private def hidden = yield
  def method_missing(n, *a, &b) = n == :ghost ? b.call(*a) : super
  def respond_to_missing?(n, p = false) = n == :ghost || super
end
b = Box.new
3.times do |i|
  GC.start if i == 1
  p b.get(:a) { 0 }, b.get(:zz) { |k| "miss #{k}" }, b.each { |x| x }, b.via { |v| v + i }, b.via, b.nilblk
  p b.map_sym([1, 2]), b.meth([3]), b.ghost(5) { |x| x * 3 }
  pr = proc { |x| x + 100 }; la = ->(x) { x - 1 }
  p b.via(&pr), b.via(&la), [1, 2].map(&pr), b.get(:q, &->(k) { k.to_s })
end
begin
  b.priv_call { 1 }
rescue NoMethodError => e
  p e.class
end
def finder(arr, &blk) = arr.each(&blk)
r = finder([1, 2, 3]) { |x| break x * 10 if x == 2 }
p r
def ret_test
  [1, 2].each(&proc { |x| return x + 50 })
  :not_here
end
p ret_test
p Array.new(2, &:itself.to_proc.then { |pr| proc { |i| i * 7 } })
class Box; def via(&b) = [:redefined, yielder(&b)]; end
p b.via { 1 }
blk = proc { |x| x.to_s * 2 }
p %w[a b].map(&blk), { k: 1 }.map(&proc { |k, v| [v, k] })
