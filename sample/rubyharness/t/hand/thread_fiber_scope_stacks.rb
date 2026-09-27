def inner(tag, q_in, q_out)
  "#{tag}-inner" =~ /(\w+)-inner/
  q_out << :entered
  q_in.pop
  $1
end
qa = Queue.new; qb = Queue.new; qa_o = Queue.new; qb_o = Queue.new
ta = Thread.new { "A-outer" =~ /(A)/; inner("a", qa, qa_o); $~[0] }
qa_o.pop                                  # A is inside inner (pushed)
tb = Thread.new { "B-outer" =~ /(B)/; inner("b", qb, qb_o); $~[0] }
qb_o.pop                                  # B is inside inner (pushed after A)
qa << :go; ra = ta.value                  # A returns first: pops B's entry
qb << :go; rb = tb.value
p ra, rb
def fm = ("fib" =~ /(f)ib/; Fiber.yield; $1)
"main" =~ /(m)ain/
f = Fiber.new { "fo" =~ /(o)/; r = fm; [r, $~[1]] }
f.resume
def mm(f) = ("zz" =~ /(z)/; r = f.resume; [r, $1])
p mm(f), $~[1]
class Kx; end
g = Fiber.new { Kx.class_eval { Fiber.yield; def from_g = 1 } }
g.resume
Kx.class_eval { def from_main = 2 }
g.resume
p Kx.instance_methods(false).sort
qa = Queue.new; qb = Queue.new; oa = Queue.new; ob = Queue.new
ta = Thread.new { begin; raise "A"; rescue; oa << 1; qa.pop; $!.message; end }
oa.pop
tb = Thread.new { begin; raise "B"; rescue; ob << 1; qb.pop; $!.message; end }
ob.pop
qa << 1; ra = ta.value
qb << 1; rb = tb.value
p ra, rb, $!
f = Fiber.new { begin; raise "F"; rescue; Fiber.yield; $!.message; end }
f.resume
r = begin; raise "M"; rescue; x = f.resume; [x, $!.message]; end
p r, $!
