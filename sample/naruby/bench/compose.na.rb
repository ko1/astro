# Composed call: f(g(h(x))).  Each level is a single call.
def h(n) = n + 1
def g(n) = h(n) * 2
def f(n) = g(n) - 3

acc = 0
i = 0
while i < 30_000_000
  acc = acc + f(10)
  i += 1
end
p acc
