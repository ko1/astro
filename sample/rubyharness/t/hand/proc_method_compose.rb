f = ->(x) { x + 1 }
g = ->(x) { x * 2 }
p (f >> g).call(3)
p (f << g).call(3)
def m1(x); x + 10; end
mm = method(:m1)
p (mm >> g).call(5)
h = {a: 1, b: 2}
p [:a, :b, :a].map(&h)
p [1, 2, 3].map(&(f >> g))
