# ObjectSpace.trace_object_allocations: file / line / class path / method id of an allocation (a method's,
# a C method's, a singleton method's, a block's), nesting, clear, the info surviving GC, `require "x.so"` for
# koruby's own lib stand-ins (2026-10-16)
require "objspace"
F = File.basename(__FILE__)
class Foo
  def m = Object.new
  def s = "ab".upcase
  def self.k = [1]
  def multi
    x = 1
    "y#{x}"
  end
end
def info(x) = [ObjectSpace.allocation_sourcefile(x)&.then { File.basename(_1) }, ObjectSpace.allocation_sourceline(x),
               ObjectSpace.allocation_class_path(x), ObjectSpace.allocation_method_id(x), ObjectSpace.allocation_generation(x).class]
r = []
ObjectSpace.trace_object_allocations do
  o = Object.new
  a = [1, 2]
  m = Foo.new.m
  u = Foo.new.s
  k = Foo.k
  b = proc { "x" + "y" }.call
  t = Foo.new.multi
  GC.start
  [o, a, m, u, k, b, t].each { |x| r << info(x) }
  r << [ObjectSpace.allocation_sourcefile(1), ObjectSpace.allocation_sourcefile(:a), ObjectSpace.allocation_sourcefile(nil)]
end
r.each { p _1 }
p ObjectSpace.allocation_sourcefile(Object.new)
kept = nil
ObjectSpace.trace_object_allocations { kept = Object.new }
p [ObjectSpace.allocation_sourceline(kept) == __LINE__ - 1]
ObjectSpace.trace_object_allocations_clear
p ObjectSpace.allocation_sourceline(kept)
# (allocated in a method: under AOT the frame that starts tracing keeps running its fused code, unrecorded)
def mk = Object.new
ObjectSpace.trace_object_allocations_start
ObjectSpace.trace_object_allocations_start
ObjectSpace.trace_object_allocations_stop
n1 = mk
ObjectSpace.trace_object_allocations_stop
ObjectSpace.trace_object_allocations_stop
n2 = mk
p [ObjectSpace.allocation_sourceline(n1), ObjectSpace.allocation_method_id(n1), ObjectSpace.allocation_sourceline(n2)]
p [require("objspace.so"), require("etc.so").class]
