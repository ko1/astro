# o.m(a) { } for a fixed-arity method that takes the block: the short invoke must
# lay the block cells out exactly as the general one, for every way the block is used
class C
  def y1(x) = yield(x)
  def y2(a, b) = yield(a) + yield(b)
  def given(x) = block_given? ? yield(x) : :none
  def capt(x, &b) = [b.call(x), b.class, b.lambda?]
  def loc(x)
    t = x * 2
    u = yield(t)
    [t, u]
  end
  def nested(x) = [1, 2].map { |i| yield(x + i) }
  def deep(n, &b) = n == 0 ? yield(:bottom) : deep(n - 1, &b)
  def rec(n) = n == 0 ? yield(0) : rec(n - 1) { |v| yield(v + 1) }
  def raiser(x) = yield(x)
  def backref(s) = (s =~ /(b+)/; yield($1))
  def h(x) = yield(x)
end
o = C.new
3.times do |i|
  p o.y1(i) { _1 + 1 }, o.y2(i, 10) { |v| v * 2 }, o.given(i) { :blk }, o.capt(i) { |v| v - 1 }, o.loc(i) { |v| v + 100 }
  p o.nested(i) { |v| v * 10 }, o.deep(3) { |v| [v, i] }, o.rec(4) { |v| v + i }
end
# break / next / return through the short path
p o.y1(1) { break :broke }
p o.y1(1) { next :nexted }
def ret_from_block(o) = (o.y1(5) { |v| return v * 3 }; :not_here)
p ret_from_block(o)
# a raise in the block keeps the method frame in the backtrace
begin
  o.raiser(1) { raise "in block" }
rescue => e
  p e.message, e.backtrace.grep(/raiser/).size >= 1
end
# $~ belongs to the method frame
"zz" =~ /z/
p o.backref("abbbc") { |m| m }, $~[0]
# a keywords-marked last argument binds positionally (no keyword params)
p o.h(k: 1) { |v| v }
# a def inside instance_eval while a block method runs
class D; def m(x) = (def made_by_m = :made; yield(x)); end
d = D.new
d.instance_eval { m(1) { |v| v } }
p d.respond_to?(:made_by_m), D.instance_methods(false).sort
# wrong arity still raises through the cached site
begin
  o.y1 { 1 }
rescue ArgumentError => e
  p e.message
end
# redefinition seen by the cached site
class C; def y1(x) = yield(x, :redef); end
p o.y1(7) { |a, b| [a, b] }
# a block that escapes as a Proc after the method returned
class E; def keep(x, &b) = (@b = b; yield(x)); def again(v) = @b.call(v); end
e = E.new
p e.keep(1) { |v| v + 1 }, e.again(41)
