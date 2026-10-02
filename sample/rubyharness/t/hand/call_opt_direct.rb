# o.m(a) for def m(a, b = expr): the defaults run in the method's scope on the
# direct path; every shape must behave as CRuby
class C
  def initialize = @base = 100
  def one(a, b = 1) = [a, b]
  def two(a, b = a * 2, c = b + @base) = [a, b, c]
  def selfy(a, b = self.class.name, c = __method__) = [a, b, c]
  def raising(a, b = raise(ArgumentError, "dflt #{a}")) = [a, b]
  def redef(a, b = (C.class_eval { def redef(*) = :new }; :dflt)) = [a, b]
  def loc(a, b = (x = 5; x + a)) = [a, b, (defined?(x) ? x : :none)]
  def blk(a, b = 2) = block_given? ? yield(a + b) : :noblk
end
o = C.new
3.times do |i|
  p o.one(i), o.one(i, 9), o.two(i), o.two(i, 1), o.two(i, 1, 2), o.selfy(i), o.loc(i)
end
begin
  o.raising(7)
rescue ArgumentError => e
  p e.message   # the backtrace label of a raising default is "<main>" here, "C#raising" in CRuby (todo.md)
end
p o.redef(1), o.redef(2)
[-> { o.one }, -> { o.one(1, 2, 3) }, -> { o.two }].each do |pr|
  pr.call
rescue ArgumentError => e
  p e.message
end
p o.blk(1) { _1 * 10 }, o.blk(1)
# def inside instance_eval while a default-arg method runs
class D; def m(a, b = 1) = (def made = :made; a + b); end
d = D.new
d.instance_eval { m(1) }
p d.respond_to?(:made), D.instance_methods(false).sort
# a kwargs-looking hash as the only optional
class E; def h(a, opts = {}) = [a, opts]; end
p E.new.h(1), E.new.h(1, x: 2)
