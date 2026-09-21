# A copy owns its singleton class.  Sharing the source's would let the copy's
# `def`/`undef` reach back into the original -- rspec-expectations dups ::RSpec
# and undefs .configuration on the dup to simulate "rspec-core not loaded".
module Foo
  def self.conf = 1
  def self.other = 2
  X = 9
end
d = Foo.dup
p [d.equal?(Foo), d.singleton_class.equal?(Foo.singleton_class)]
class << d; undef conf; end
p [Foo.respond_to?(:conf), d.respond_to?(:conf), Foo.conf]
p [d.name, d.other, d::X]

class A; def self.cm = :cm; end
class B < A; def self.bm = :bm; end
c = B.clone
p [c.cm, c.bm, c.singleton_class.equal?(B.singleton_class), c.superclass]
p [B.singleton_class.superclass, c.singleton_class.superclass]
class << c; undef bm; end
p [B.bm, (c.bm rescue $!.class)]

# `extend`ed modules come along with the copy's singleton
module Ext; def hello = :hello; end
module Src; extend Ext; end
p Src.dup.hello
p Src.clone.hello

# an ordinary object: clone carries the singleton, dup drops it, and neither
# copy shares a method table with the original
o = Object.new
def o.x = 1
cl = o.clone
def cl.y = 2
p [o.respond_to?(:y), cl.respond_to?(:x), cl.singleton_class.equal?(o.singleton_class)]
d2 = o.clone
class << d2; undef x; end
p [o.respond_to?(:x), d2.respond_to?(:x)]
p o.dup.respond_to?(:x)

# frozen source still clones its singleton
f = Object.new
def f.z = 3
f.freeze
g = f.clone
p [g.z, g.frozen?, f.clone(freeze: false).frozen?]

# constants defined in `class << obj` come along with the copy
s = "x"
class << s; K = :k; end
p s.clone.singleton_class::K
module Sc; class << self; SK = :sk; end; end
p [Sc.dup.singleton_class::SK, Sc.clone.singleton_class::SK]
