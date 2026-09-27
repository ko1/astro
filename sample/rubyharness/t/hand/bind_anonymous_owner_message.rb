# UnboundMethod#bind / #bind_call name an anonymous owner as #<Class:0x…> in their TypeError.
def t = (yield rescue [$!.class, $!.message.gsub(/0x\h+/, "X")])
k = Class.new { def hey = 1 }
p t { k.instance_method(:hey).bind_call(Object.new) }, t { k.instance_method(:hey).bind(Object.new) }
m = Module.new { def hoy = 2 }
p t { m.instance_method(:hoy).bind_call(1) }, m.instance_method(:hoy).bind_call(Object.new)
class Named; def x = 3; end
p t { Named.instance_method(:x).bind(1) }
