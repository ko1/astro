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
