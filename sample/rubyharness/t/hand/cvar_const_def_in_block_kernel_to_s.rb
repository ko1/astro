class Tst
  def run
    h = {a: 1}
    k = Class.new do
      @@h = h
      def m = @@h
    end
    [k.new.m, Tst.class_variables, k.class_variables]
  end
end
p Tst.new.run
class Tst2
  X = :tst
  def run
    k = Class.new do
      const_set(:X, :anon)
      def m = X
      define_method(:dm) { X }
    end
    [k.new.m, k.new.dm]
  end
end
p Tst2.new.run
k2 = Class.new { X = 5 rescue nil; def z = 1 }; p k2.new.z
class A; def self.mk = Class.new { def q = self.class.name }; end; p A.mk.new.q
class Base3; @@x = :base; end
Base3.class_eval("def ev = @@x")
p Base3.new.ev
class Base3; def sp = Class.new { def g = @@x }.new.g; end
p Base3.new.sp
class KC; def inspect = "CUSTOM"; def to_s = "TS"; end
o = KC.new
p Kernel.instance_method(:to_s).bind_call(o).sub(/0x\h+/, "X"), Kernel.instance_method(:inspect).bind_call(o).sub(/0x\h+/, "X")
class KD; def inspect = "#<D #{Kernel.instance_method(:to_s).bind_call(self).sub(/0x\h+/, "X")}>"; end
p KD.new.inspect
