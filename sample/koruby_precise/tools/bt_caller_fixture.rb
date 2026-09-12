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
