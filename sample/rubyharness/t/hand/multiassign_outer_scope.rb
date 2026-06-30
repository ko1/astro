def counter
  a, b = 1, 1
  [1, 2, 3].each { a, b = b, a + b }
  [a, b]
end
p counter
x, y = 10, 20
[1].each { x, y = y, x }
p [x, y]
def fib_n(n)
  a, b = 0, 1
  r = []
  n.times { r << a; a, b = b, a + b }
  r
end
p fib_n(10)
sum = 0; prod = 1
[1, 2, 3, 4].each { |i| sum, prod = sum + i, prod * i }
p [sum, prod]
p, q, r = 1, 2, 3
[1].each { p, q, r = r, p, q }
[p, q, r]
