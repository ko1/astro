# caller(0) shapes compared against CRuby (tools/bt_caller_cmp.sh).
# Prints one block of frames per scenario; the runner strips the leading
# directory so the two interpreters' output is comparable verbatim.

def show(tag)
  puts "== #{tag}"
  caller(0).each { |l| puts l }
  puts
end

# 1. plain method nesting
def lvl3 = show("nested")
def lvl2 = lvl3
def lvl1 = lvl2
lvl1

# 2. a literal block
def with_block
  [1].each { show("block-each") }
end
with_block

# 3. block nested two levels
def nested_blocks
  [1].each { [2].each { show("block-2levels") } }
end
nested_blocks

# 4. yield
def yielder
  yield
end
def uses_yield
  yielder { show("yield") }
end
uses_yield

# 5. Proc#call
def proc_caller
  pr = proc { show("proc-call") }
  pr.call
end
proc_caller

# 6. lambda call
def lambda_caller
  l = lambda { show("lambda-call") }
  l.call
end
lambda_caller

# 7. instance_exec
class Host
  def run
    instance_exec { show("instance-exec") }
  end
end
Host.new.run

# 8. super
class Base
  def go = show("super-base")
end
class Derived < Base
  def go = super
end
Derived.new.go

# 9. block inside a method called through a yield chain
def outer_yield
  yield 1
end
def middle
  outer_yield { |x| [x].map { show("yield-then-block") } }
end
middle

# 10. a block passed to a C method that takes a block (map over 2 elements)
def c_method_block
  [1, 2].map { |x| x == 1 ? show("map-block") : x }
end
c_method_block

# 11. define_method body
class DM
  define_method(:dm) { show("define-method") }
end
DM.new.dm

# 12. top level block
[1].each { show("toplevel-block") }
show("toplevel")

# 13. class / module / singleton-class bodies
class CB
  show("class-body")
  [1].each { show("class-body-block") }
end
module MB
  show("module-body")
end
class SC
  class << self
    show("sclass-body")
  end
end
class Nest
  class Inner
    show("nested-class-body")
  end
end
module Outer
  module Inner2
    show("nested-module-body")
  end
end
OBJ = Object.new
class << OBJ
  show("sclass-of-object")
end

# 14. method_missing
class MM
  def method_missing(name, *a)
    show("method-missing")
    helper
  end
  def respond_to_missing?(name, include_private = false) = true
  def helper = show("mm-helper")
end
MM.new.nope

# 15. method_missing reached through send
class MM2
  def method_missing(name, *a) = show("mm-via-send")
end
MM2.new.send(:zork)

# 16. method_missing with a block
class MM3
  def method_missing(name, *a, &b)
    [1].each { show("mm-block") }
  end
end
MM3.new.zonk { }

# 17. super into method_missing
class MMBase
  def method_missing(name, *a) = show("mm-super-base")
end
class MMDer < MMBase
  def method_missing(name, *a) = super
end
MMDer.new.quux

# 18. splat call
def splatted(a, b) = show("splat-call")
ARGS = [1, 2]
splatted(*ARGS)

# 19. instance_exec with args, and instance_eval
class IE
  def run2 = instance_exec(1) { |x| show("instance-exec-arg") }
  def run3 = instance_eval { show("instance-eval") }
end
IE.new.run2
IE.new.run3

# 20. Proc#call through a method, and Method#call
def pc
  p2 = proc { show("proc-call-nested") }
  p2.call
end
pc
def mc = show("method-obj-call")
method(:mc).call

# 21. define_method with args, and define_method in a module
class DM2
  define_method(:dm2) { |x| show("define-method-arg") }
end
DM2.new.dm2(1)
module DMM
  define_method(:dmm) { show("define-method-module") }
end
class DMUser
  include DMM
end
DMUser.new.dmm

# 22. define_method inside a singleton class body
class DM3
  class << self
    define_method(:dm3) { show("define-method-sclass") }
  end
end
DM3.dm3

# 23. caller_locations shape
def loc_show(tag)
  puts "== #{tag}"
  caller_locations(0).each { |l| puts "#{l.path}:#{l.lineno}:in '#{l.label}'" }
  puts
end
class LC
  loc_show("locations-class-body")
  define_method(:lc) { loc_show("locations-define-method") }
end
LC.new.lc
class LMM
  def method_missing(n, *a) = loc_show("locations-method-missing")
end
LMM.new.blah
