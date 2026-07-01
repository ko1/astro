# Modules (incl. Kernel) respond to the lifecycle hooks (method_added/included/
# prepended/extended) with default no-ops, and a module value dispatches
# Module-level methods (ancestors/name/method_added). vs ruby.
p Kernel.ancestors
p Kernel.name
p Kernel.respond_to?(:method_added, true)

log = []
mod = Module.new do
  define_singleton_method(:method_added) { |n| log << n }
  def one; end
  def two; end
end
p log

included_base = nil
m2 = Module.new do
  define_singleton_method(:included) { |base| included_base = base }
end
klass = Class.new { include m2 }
p included_base.equal?(klass)

p Kernel.Integer("42")
p Kernel.Float("2.5")
