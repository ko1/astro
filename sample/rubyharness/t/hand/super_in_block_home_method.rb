# `super` in a block continues above the block's home method, even when that
# method was itself reached by `super` (it used to re-resolve the name on the
# receiver and recurse forever).
class Base; def t = "base"; def u(x) = "u#{x}"; end
module M; def t = w { super() }; def w = yield; end
class X < Base; include M; def t = "X>" + super; end
p X.new.t
module V; def new(*a) = super(*a); end
class P
  def self.new(s) = [1].map { super(s) }.first
  def initialize(s) = @s = s
  attr_reader :s
end
class Q < P; extend V; end
p Q.new("b").s
class Y < Base
  def u(x) = [1].map { [2].each_with_object([]) { |_, a| a << super(x) } }.flatten.first
  def keep = proc { super() rescue $!.class }
  def t = keep
end
class Z < Y; def u(x) = "Z" + super; end
p Z.new.u(3), Y.new.t.call
class W < Base; define_method(:t) { [0].map { super() }.first + "!" }; end
class W2 < W; def t = "W2>" + super; end
p W2.new.t
p((proc { super() }.call rescue $!.class))
