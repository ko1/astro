# a String method/constant name keeps its encoding (define_singleton_method, send, const_set/get/defined?)
o = Object.new
n = "tq".encode("UTF-16LE")
o.define_singleton_method(n) { 1 }
p o.send(n), o.respond_to?(n), o.respond_to?(:tq), o.singleton_methods.map(&:encoding)
o.define_singleton_method("b\xff".b) { 2 }
p o.singleton_methods.map(&:encoding).sort_by(&:name)
module ME; end
e = "Xλ".encode("euc-jp")
ME.const_set(e, 3)
p ME.const_get(e), ME.const_get(e.to_sym), ME.const_defined?(e), ME.constants.map(&:encoding)
# a multibyte class name round-trips through Marshal
module ME; class Multiぁあ; end; end
p Marshal.load(Marshal.dump(ME::Multiぁあ))
# Class#new is listed by reflection though it has no method-table entry
p String.methods.include?(:new), Class.instance_methods(false).include?(:new), Class.public_method_defined?(:new), String.singleton_class.method_defined?(:new), Module.method_defined?(:new)
