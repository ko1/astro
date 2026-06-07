# L1: closures, scope capture, currying, higher-order
def make_adder(n)
  ->(x) { x + n }
end
add5 = make_adder(5)
add10 = make_adder(10)
p add5.call(1)
p add10.call(1)
p add5.call(add10.call(0))

def make_counter
  c = 0
  inc = -> { c += 1 }
  get = -> { c }
  [inc, get]
end
inc, get = make_counter
inc.call
inc.call
p get.call

# shared capture between two blocks
total = 0
adder = ->(x) { total += x }
[1, 2, 3, 4].each(&adder)
p total

# block modifying outer var
acc = []
[1, 2, 3].each { |x| acc << x * x }
p acc

# curry
mul = ->(a, b, c) { a * b * c }
p mul.curry[2][3][4]
p mul.curry[2, 3][4]

add = ->(a, b) { a + b }
inc2 = add.curry[1]
p inc2[10]

# method reference / to_proc
p [1, -2, 3, -4].map(&:abs)
upper = "hello".method(:upcase)
p upper.call

# proc vs lambda arity
l = ->(a, b) { [a, b] }
p l.arity
pr = proc { |a, b| [a, b] }
p pr.arity
p pr.call(1)
p pr.call(1, 2, 3)

# yield with closure
def repeat(n)
  n.times { |i| yield i }
end
out = []
repeat(3) { |i| out << i }
p out

# returning lambda from method, called later
fns = (1..3).map { |n| ->(x) { x * n } }
p fns.map { |f| f.call(10) }

# nested closures
def outer
  x = 1
  inner = lambda do
    y = 2
    -> { x + y }
  end
  inner.call.call
end
p outer

compose = ->(f, g) { ->(x) { f.call(g.call(x)) } }
inc_fn = ->(x) { x + 1 }
dbl_fn = ->(x) { x * 2 }
p compose.call(inc_fn, dbl_fn).call(5)
