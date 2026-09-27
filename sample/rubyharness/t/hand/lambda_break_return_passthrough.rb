# A lambda passes on a break that a block it called claimed (it ends that block's call,
# not the lambda), and a lambda yielded to (map(&lam)) ends at its own return — which
# used to escape to the top level and end the program silently.
def mid(&b)
  -> { $r << :before; b.call; $r << :unreachable1 }.call
  $r << :unreachable2
end
$r = []
res = [1].each { |e| mid { break }; $r << :after }
p res, $r
l = -> { break 5; 6 }; p l.call
l2 = lambda { [1, 2].each { |x| break x * 7 }; :after_each }; p l2.call
l3 = -> { 1.times { break :inner }; :l3 }; p l3.call
def takes_lambda = yield
p takes_lambda(&-> { break :lb })
m = ->(x) { return x * 2 }; p [1, 2].map(&m)
p [1, 2, 3].each(&->(x) { break :stop if x == 2 })
def meth_with_proc = [1, 2].each { |x| return :from_meth if x == 2 }
p meth_with_proc
lam_nested = -> { [1, 2].each { |x| return :lam_ret if x == 2 }; :no }; p lam_nested.call
def yl = yield(3)
p yl(&->(x) { return x + 1 })
p [[1, 2]].map(&->(a) { return a })
def outer_m = [1].map(&->(x) { [2].each { |y| return x + y } })
p outer_m
