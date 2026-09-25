# A closure over a local declared inside a string eval must survive the eval's
# frame (its env is closed when the eval returns), not read a dead stack.
def f; eval("y = 3; proc { y }"); end
def f2; eval("y = [3]; proc { y }"); end
def g(a, b, c, d, e); [a, b, c, d, e]; end
pr = f; g(1,2,3,4,5); p pr.call
pr2 = f2; 3.times { g(:a, :b, :c, :d, :e) }; GC.start; p pr2.call
def f3; Object.new.instance_eval("q = [4]; proc { q }"); end
pr = f3; 3.times { g(1, 2, 3, 4, 5) }; GC.start; p pr.call
def f4; b = binding; b.eval("r = [5]; proc { r }"); end
pr = f4; 3.times { g(1, 2, 3, 4, 5) }; GC.start; p pr.call
def f5; eval("s = [6]; -> { s << 1; s }"); end
l = f5; 3.times { g(1, 2, 3, 4, 5) }; p l.call, l.call
