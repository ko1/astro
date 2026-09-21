# `super` written inside a block still forwards the enclosing METHOD's block.
class A
  def m(key) = (block_given? ? "A-#{yield(key)}" : "A-noblk")
  def n(key, &b) = (b ? b.call(key) : "n-noblk")
  def q = "A#q"
end
class B < A
  def m(key)
    [1].each { return super }           # bare super in a block
  end
  def n(key)
    [1].map { super }.first
  end
  def q = [1].map { super }.first
end
p B.new.m(:k) { |x| "blk-#{x}" }
p B.new.n(:k) { |x| "blk2-#{x}" }
p B.new.q
p B.new.m(:k)

class C < A
  def m(key)
    r = nil
    [1].each { r = super(key) }          # explicit-arg super in a block
    r
  end
end
p C.new.m(:k) { |x| "c-#{x}" }
p C.new.m(:k)

class D < A                              # two blocks deep
  def m(key) = [1].map { [2].map { super(key) }.first }.first
end
p D.new.m(:k) { |x| "d-#{x}" }

class E < A                              # a lambda, called immediately
  def m(key) = lambda { super }.call
end
p E.new.m(:k) { |x| "e-#{x}" }

class F < A                              # the block explicitly dropped
  def m(key) = [1].map { super(key, &nil) }.first
end
p F.new.m(:k) { |x| "f" }

class G < A                              # a Proc that outlives the method frame
  def m(key) = proc { super(key) }
end
g = G.new.m(:k) { |x| "g-#{x}" }
p (g.call rescue [$!.class, $!.message[0, 40]])

# the shape concurrent-ruby uses: a mutex wrapper delegating to super
require 'monitor'
class Backend
  def compute(key) = yield(key)
end
class Locked < Backend
  def initialize = @lock = Monitor.new
  def compute(key) = @lock.synchronize { super }
end
p Locked.new.compute(:x) { |k| "computed-#{k}" }
