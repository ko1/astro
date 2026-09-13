def show(tag, list)
  puts "== #{tag}"
  (list || ['<nil>']).each { |l| puts "  #{l}" }
end

# A. stale carry where the dead frame's identity cell survives: the yield that
#    raised sat deep in a staging (call args), the later block call sits shallow.
def y0; yield; end
def foo(*a); a; end
def goA
  begin
    foo(1, 2, 3, 4, y0)
  rescue LocalJumpError
  end
  [1].each { show('A stale carry deep', caller(0)) }
end
goA

# B. same with a method that yields to a lambda with wrong arity
def y2; yield 1, 2; end
def goB
  begin
    foo(1, 2, 3, 4, y2(&->(a) { a }))
  rescue ArgumentError
  end
  [1].each { show('B stale carry lambda', caller(0)) }
end
goB

# C. bind_call: the method frame's EP cell is the bind target
class BC2
  def m(a); pr = proc { a }; [binding, pr]; end
end
b, pr = BC2.instance_method(:m).bind_call(BC2.new, 11)
p b.local_variables
p b.local_variable_get(:a)
p pr.call
p eval('a', pr.binding)
GC.start
p pr.call
b2, pr2 = BC2.instance_method(:m).bind(BC2.new).call(12)
p b2.local_variables, pr2.call

# D. a big Integer staged just below a C-built [recv, arg] window
class Eq2; def ==(o); show('D == big int', caller(0)); false; end; end
[0x2AAA_AAAA_AAAA, 0x3FFF_FFFF_FFF9, 0x3FFF_FFFF_FFFD, 1 << 45, (1 << 46) + 4].each do |big|
  [big, 2].index(Eq2.new)
  [Eq2.new, big].include?(big)
  { big => 1 }.key?(Eq2.new) rescue nil
end
puts "D done"
