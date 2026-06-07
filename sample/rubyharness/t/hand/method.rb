# L1: method definition and argument forms
def add(a, b)
  a + b
end
p add(2, 3)

def greet(name = "world")
  "hi #{name}"
end
p greet
p greet("ruby")

def total(*nums)
  nums.sum
end
p total
p total(1, 2, 3)

def first_rest(head, *tail)
  [head, tail]
end
p first_rest(1, 2, 3, 4)

def kw(a:, b: 10)
  a + b
end
p kw(a: 1)
p kw(a: 1, b: 2)

def opts(**h)
  h
end
p opts(x: 1, y: 2)

def mixed(a, b = 2, *c, d:, **e)
  [a, b, c, d, e]
end
p mixed(1, d: 4)
p mixed(1, 2, 3, 4, d: 5, z: 6)

def apply(x)
  yield x
end
p apply(10) { |n| n * 2 }

def explicit(&blk)
  blk.call(5)
end
p explicit { |n| n + 1 }

def with_block?
  block_given?
end
p with_block?
p(with_block? { })

def fib(n)
  n < 2 ? n : fib(n - 1) + fib(n - 2)
end
p fib(10)

def fact(n)
  return 1 if n <= 1
  n * fact(n - 1)
end
p fact(5)

def multi
  return 1, 2, 3
end
p multi

def splat_call(a, b, c)
  a + b + c
end
args = [1, 2, 3]
p splat_call(*args)

def takes_hash(a, h)
  [a, h]
end
p takes_hash(1, x: 2, y: 3)

x = 100
forward = ->(n) { n + x }
p forward.call(1)
