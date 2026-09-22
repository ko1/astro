# A dup'd / cloned Module or Class gets its OWN mutable tables: class ivars,
# class variables and the included/prepended module lists.  Sharing them meant
# a write through the copy changed the original (dry-container's `.dup` gives
# each copy its own registry exactly this way).
K = Class.new
K.instance_variable_set(:@t, [1])
k2 = K.dup
p [K.instance_variable_get(:@t), k2.instance_variable_get(:@t)]
p k2.instance_variable_get(:@t).equal?(K.instance_variable_get(:@t))
k2.instance_variable_set(:@t, [9])
p [K.instance_variable_get(:@t), k2.instance_variable_get(:@t)]

C = Class.new
C.class_variable_set(:@@v, 1)
c2 = C.dup
c2.class_variable_set(:@@v, 2)
p [C.class_variable_get(:@@v), c2.class_variable_get(:@@v)]

module Mod1; end
module Mod2; end
B = Class.new { include Mod1 }
b2 = B.dup
b2.include(Mod2)
p [B.include?(Mod1), B.include?(Mod2), b2.include?(Mod1), b2.include?(Mod2)]
b3 = B.clone
b3.prepend(Mod2)
p [B.ancestors.include?(Mod2), b3.ancestors.include?(Mod2)]

# a module that overrides #dup and copies state in it (dry-container's mixin)
module Registry
  def dup
    copy = super
    copy.instance_variable_set(:@reg, reg.dup)
    copy
  end
  def clone
    copy = super
    copy.instance_variable_set(:@reg, reg.dup) unless copy.frozen?
    copy
  end
  def reg = (@reg ||= {})
end

Holder = Class.new { extend Registry }
Holder.reg[:a] = 1
h2 = Holder.dup
h2.reg[:b] = 2
p [Holder.reg, h2.reg]
h3 = Holder.clone
h3.reg[:c] = 3
p [Holder.reg, h3.reg]

# methods and constants still come along
Src = Class.new do
  CONST_IN_SRC = 7 unless defined?(CONST_IN_SRC)
  def hello = :hi
  def self.shout = :HI
end
d = Src.dup
p [d.new.hello, d.shout, d.name]
