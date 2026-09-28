# Kernel.define_method lands on Kernel (main's define_method must not capture it); aliasing back restores the original
def without_rdoc
  ::Kernel.send(:alias_method, :irb_original_require, :require)
  ::Kernel.define_method(:require) do |name|
    raise LoadError, "no rdoc" if name.match?("rdoc")
    ::Kernel.send(:irb_original_require, name)
  end
  yield
ensure
  ::Kernel.send(:alias_method, :require, :irb_original_require)
  ::Kernel.undef_method :irb_original_require
end
2.times { without_rdoc { p require("set"); begin; require("rdoc"); rescue LoadError => e; p e.message; end; p require("json") } }
p require("prettyprint")
Kernel.define_method(:q2) { 2 }
p Object.instance_methods(false).include?(:q2), Kernel.instance_methods(false).include?(:q2)
define_method(:x) { 1 }; p x, Object.public_method_defined?(:x)
p((1.define_method(:y) {} rescue $!.class))
