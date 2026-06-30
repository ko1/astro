module M; def hi; :hi; end; def ho; :ho; end; end
def takes(x); x.name; end
p takes(M)
p [M, Comparable, Enumerable].map(&:name)
p M.ancestors
p M.instance_methods(false).sort
p M.method_defined?(:hi)
p M.method_defined?(:nope)
p [M, Integer].map { |m| m.name }
runtime_mod = Module.new
runtime_mod.module_eval { def dyn; 1; end }
p runtime_mod.instance_methods
p runtime_mod.name
p (Comparable < Object)
p (Integer < Numeric)
p M.const_set(:X, 42)
p M::X
p M.const_defined?(:X)
p M.const_get(:X)
module Self2
  def self.describe; "#{name}: #{instance_methods(false).size} methods"; end
  def a; end
  def b; end
end
p Self2.describe
p [Comparable, Enumerable, Kernel].sort_by(&:name).map(&:name)
module Helper
  def self.included(base); base.class_eval { def helped; :yes; end }; end
end
class Host; include Helper; end
p Host.new.helped
p M.is_a?(Module)
p M.instance_of?(Module)
p Integer.is_a?(Module)
p [String, Array, Hash].map { |c| c.instance_methods(false).size > 0 }
