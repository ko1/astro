# recv.m(pos..., k: v...) with literal symbol keys: hash-free binding when the
# callee allows, the kwargs Hash otherwise — every shape must behave as CRuby
class K
  def req(a, k:) = [a, k]
  def opt(a, k: 1, j: 2) = [a, k, j]
  def rest(a, k: 1, **r) = [a, k, r]
  def nokw(a, h = nil) = [a, h]
  def nokw_rest(*xs) = xs
  def nilkw(a, **nil) = a
  def blk(a, k: 0) = yield(a + k)
  def method_missing(n, *a, **kw) = n.to_s.start_with?("mm_") ? [n, a, kw] : super
  def respond_to_missing?(n, p = false) = n.to_s.start_with?("mm_") || super
  def call_prot(o) = o.prot(1, k: 2)
  protected def prot(a, k:) = [:prot, a, k]
  private def priv(a, k:) = [:priv, a, k]
end
o = K.new
3.times do |i|
  p o.req(i, k: :x), o.opt(i, j: 9), o.opt(i, k: 5, j: 6), o.rest(i, z: 1, k: 2), o.nokw(i, k: 3), o.nokw_rest(i, k: 4)
  p o.blk(i, k: 10) { _1 * 2 }, o.mm_foo(i, k: 1), o.call_prot(K.new)
end
[-> { o.req(1) }, -> { o.req(1, k: 1, x: 2) }, -> { o.nilkw(1, k: 1) }, -> { o.priv(1, k: 2) },
 -> { o.prot(1, k: 2) }, -> { o.nope(1, k: 2) }, -> { o.opt(1, 2, k: 3) }].each do |pr|
  pr.call
rescue ArgumentError, NoMethodError => e
  p [e.class, e.message]
end
# C methods with keywords
p 2.5.round(half: :even), 3.5.round(half: :even), [3, 1, 2].sum(0), "a-b".split("-", 2)
p [{ a: 1 }.merge({ b: 2 }) { |*| 0 }, 1234.digits(10)]
# a builtin receiver with a Ruby method taking keywords
class String
  def wrap(l: "[", r: "]") = l + self + r
end
p "x".wrap, "y".wrap(r: ">"), "z".wrap(l: "<", r: ">")
# ruby2_keywords passes the keywords on as a flagged Hash
class K
  ruby2_keywords def fwd(*args) = req(*args)
end
p o.fwd(7, k: 8)
# redefinition between calls is seen
class K
  def opt(a, k: 1, j: 2) = [:redef, a, k, j]
end
p o.opt(1, k: 2)
