# A singleton class shows a class/module attachee by its own #inspect; Kernel#to_s
# / #inspect bound to a class give the generic #<Class:0x...> form.
def self.hide(s) = s.gsub(/0x[0-9a-f]+/, "X")
m = Module.new { def inspect(v = 1) = "MI#{v}"; module_function :inspect }
puts [m.singleton_class.to_s].map { |x| hide(x.to_s) }.inspect
m2 = Module.new { def self.inspect = "Custom" }
puts [m2.singleton_class.to_s, m2.singleton_class.inspect].map { |x| hide(x.to_s) }.inspect
o = Object.new; def o.inspect = "OI"
puts [o.singleton_class.to_s].map { |x| hide(x.to_s) }.inspect
puts [Object.instance_method(:to_s).bind_call(Object.new.singleton_class).sub(/0x\h+/, "X")].map { |x| hide(x.to_s) }.inspect
puts [Object.instance_method(:to_s).bind_call(String).sub(/0x\h+/, "X")].map { |x| hide(x.to_s) }.inspect
puts [Object.instance_method(:inspect).bind_call(String.singleton_class).sub(/0x\h+/, "X")].map { |x| hide(x.to_s) }.inspect
m3 = Module.new { def self.inspect(x) = x }
p((m3.singleton_class.to_s rescue $!.class))
puts [String.singleton_class.to_s, String.singleton_class.singleton_class.to_s].map { |x| hide(x.to_s) }.inspect
