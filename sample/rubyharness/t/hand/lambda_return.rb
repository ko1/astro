l = lambda { |x| return x * 2 }
p l.call(5)
l2 = ->(x) { return x + 1 }
p l2.call(5)
l3 = ->(x) { return x if x > 0; -x }
p l3.call(5)
p l3.call(-5)
l4 = lambda { |a, b| return a + b; 999 }
p l4.call(3, 4)
def use_lambda
  f = ->(x) { return x * 10 }
  result = f.call(5)
  result + 1
end
p use_lambda
multi = ->(x) { return :neg if x < 0; return :zero if x == 0; :pos }
p multi.call(-1)
p multi.call(0)
p multi.call(1)
pr = proc { |x| x * 2 }
p pr.call(5)
counter = ->(n) { return 0 if n <= 0; n }
p [counter.call(-1), counter.call(5)]
adder = ->(a, b = 10) { return a + b }
p adder.call(5)
p adder.call(5, 20)
loop_lambda = ->(arr) {
  sum = 0
  arr.each { |x| sum += x }
  return sum
}
p loop_lambda.call([1, 2, 3, 4])
