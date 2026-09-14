# Block frames on the UNWIND path: a raise that leaves a block must leave a
# 'block in ...' frame behind, and the frame below it must be reported at the
# YIELD site, not at the line inside the block.  Run under CRuby and koruby and
# diff.  Every scenario keeps the rescue outside the block so the whole chain
# travels the unwind path (vm->bt), not the live walk.
D = __dir__

def show(tag)
  yield
rescue => e
  puts "== #{tag}"
  e.backtrace.each { |l| puts "  #{l.sub(D + '/', '')}" }
end

def deep; raise 'x'; end

# 1. yield from a Ruby method
def y1; yield; end
show('1 yield') { y1 { deep } }

# 2. block run by a C method
show('2 Array#each') { [1].each { deep } }

# 3. nested blocks -> 'block (2 levels) in ...'
show('3 nested') { [1].each { [2].each { deep } } }

# 4. block written inside a method -> 'block in Object#m4'
def m4; [1].each { deep }; end
show('4 in method') { m4 }

# 5. block written inside a class body method -> 'block in Foo#bar'
class Foo
  def bar; [1].each { raise 'y' }; end
end
show('5 in class') { Foo.new.bar }

# 6. class body block
show('6 class body') { Class.new { [1].each { raise 'z' } } }

# 7. Proc#call
pr = proc { deep }
show('7 proc.call') { pr.call }

# 8. lambda call
lm = -> { deep }
show('8 lambda') { lm.call }

# 9. yield from inside a block (node_yield_outer)
def y9; [1].each { yield }; end
show('9 yield in block') { y9 { deep } }

# 10. block passed on with &b
def y10(&b); [1].each(&b); end
show('10 &b forward') { y10 { deep } }

# 11. instance_exec
show('11 instance_exec') { Object.new.instance_exec { deep } }

# 12. Integer#times
show('12 times') { 1.times { deep } }

# 13. map inside map
show('13 map map') { [1].map { |a| [2].map { |b| deep } } }

# 14. define_method body
class Bar
  define_method(:dm) { deep }
end
show('14 define_method') { Bar.new.dm }

# 15. Symbol#to_proc (no frame of its own in CRuby)
show('15 sym to_proc') { [nil].each(&:fetch) }

# 16. a method called from a block, raising deeper
def d16; deep; end
def m16; [1].each { d16 }; end
show('16 two deep') { m16 }

# 17. raise directly in a block at top level (no enclosing method)
show('17 top block') { [1].each { raise 'q' } }

# 18. block in a block in a method
def m18; [1].each { [2].each { deep } }; end
show('18 method 2 levels') { m18 }

# 19. ensure/rescue that re-raises out of a block
def m19; [1].each { begin; deep; ensure; nil; end }; end
show('19 ensure') { m19 }

# 20. Enumerable method with a block
show('20 inject') { [1, 2].inject { |a, b| deep } }
