# Generators driven inside other generators, external enumeration in between
# (biz: a lazy chain over an Enumerator whose block pulls another one with #next).

def inner = (1..10**6).lazy.flat_map { |w| [w, w + 100] }.to_enum

# first(n) is not swallowed by a `loop` in the generator body (it used to
# yield one element too many)
pe = inner
lin = Enumerator.new { |yy| loop { yy << pe.next }; loop { yy << pe.next } }
p lin.lazy.flat_map { |x| [x, -x] }.first(4)

# the pulled enumerator survives, and the chain can be driven again
pe = inner
lin = Enumerator.new { |yy| loop { yy << pe.next } }
o = lin.lazy.flat_map { |x| [x, -x] }
p o.first(2), pe.peek
r = []; o.each { |x| r << x; break if r.size >= 2 }
p r, o.first(2)

# a break from the consumer's block passes through the generator's own proc
e = Enumerator.new { |y| y << 1; y << 2 }
r = []; e.lazy.flat_map { |x| [x, -x] }.each { |x| r << x; break if r.size >= 3 }
p r
p Enumerator.new { |y| lin2 = Enumerator.new { |z| z << :a; z << :b }; lin2.each { |x| y << x } }.first(1)

# ...while a proc's own break after its method returned is still a LocalJumpError
def mk_proc = proc { break 5 }
begin; mk_proc.call; rescue LocalJumpError => ex; p [ex.reason, ex.exit_value]; end

# ±Infinity limits: endless one way, empty the other
p 2006.downto(-Float::INFINITY).first(3), 5.upto(-Float::INFINITY).to_a, 5.downto(Float::INFINITY).to_a
r = []; 3.downto(-Float::INFINITY) { |x| r << x; break if r.size > 2 }; p r
p [1, 2, 3].zip(1.upto(Float::INFINITY)), 1.upto(Float::INFINITY).first(2), 1.upto(Float::INFINITY).size
p 5.downto(-Float::INFINITY).size

# ArithmeticSequence external iteration
a = 1.step(10, 3); p a.next, a.next, a.peek; a.rewind; p a.next
b = (1..2).step(1); p b.next, b.next; begin; b.next; rescue StopIteration => ex; p ex.class; end
