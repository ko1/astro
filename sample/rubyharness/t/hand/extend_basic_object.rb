# extend on a BasicObject (via Kernel#extend.bind_call) — extend_object must not call
# the object's own frozen?/singleton_class; a C frame called from C shows the Ruby caller's line
module Ext; def hi = :hi; end
class D < BasicObject
  def extend(*m) = ::Kernel.instance_method(:extend).bind_call(self, *m)
end
d = D.new
d.extend(Ext); p d.hi
class F < BasicObject; end
f = F.new
p ::Kernel.instance_method(:frozen?).bind_call(f)
p ::Kernel.instance_method(:class).bind_call(f)
begin; f.extend(Ext); rescue NoMethodError => x; p x.name; end
e = D.new; ::Kernel.instance_method(:freeze).bind_call(e)
begin; e.extend(Ext); rescue => x; p x.class, x.message.sub(/0x\h+/, "X"), x.receiver.equal?(e); end
class D < BasicObject
  def extend(*m) = ::Kernel.instance_method(:extend).bind_call(self, *m)
end
d = D.new
::Kernel.instance_method(:freeze).bind_call(d)
begin
  d.extend(Ext)
rescue ::Exception => e
  puts e.backtrace
end
o = Object.new.freeze
begin; o.extend(Ext); rescue => e; puts e.backtrace; end
begin; ::Kernel.instance_method(:extend).bind_call(o, Ext); rescue => e; puts e.backtrace; end
