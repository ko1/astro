# super into a simple / block-taking / optional-args parent, and Klass.new into an
# optional-args initialize: the short invokes must behave as the general one
class P
  def simple(a) = [:p_simple, a, self.class]
  def blk(a) = yield(a, :p_blk)
  def opt(a, b = a * 2, c = __method__) = [:p_opt, a, b, c]
  def raiser(a) = raise(ArgumentError, "p #{a}")
  def ret(a) = (return a * 3; :never)
end
class Q < P
  def simple(a) = super
  def blk(a) = super { |x, y| [x, y, :q] }
  def blk2(a, &b) = method(:blk).super_method.call(a, &b)
  def opt(a) = super
  def raiser(a) = super
  def ret(a) = super
end
class Q2 < P; def opt(a) = super(a, 1); end
q = Q.new
3.times do |i|
  p q.simple(i), q.blk(i), q.opt(i), Q2.new.opt(i), q.ret(i)
end
begin
  q.raiser(5)
rescue ArgumentError => e
  p e.message, e.backtrace_locations.map(&:label).first(2)
end
# arity errors through super
class R < P; def simple(a, b) = super(a, b); end
begin
  R.new.simple(1, 2)
rescue ArgumentError => e
  p e.message
end
# Klass.new → initialize with optional args (defaults read self and earlier params)
class N
  def initialize(a, b = a + 1, c = self.class.name) = (@v = [a, b, c])
  attr_reader :v
end
class M < N; end
3.times { |i| p N.new(i).v, N.new(i, 0).v, M.new(i).v }
begin
  N.new
rescue ArgumentError => e
  p e.message
end
# an initialize default that raises
class O; def initialize(a, b = raise("dflt #{a}")) = nil; end
begin
  O.new(1)
rescue => e
  p e.message
end
# super from a block-taking method into a simple parent that ignores the block
class S < P; def simple(a) = super { :ignored }; end
p S.new.simple(9)
