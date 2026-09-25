# Code in a string eval reaches the caller's locals through the binding's env:
# closures made in the string share them (writes land, later writes are seen).
x = 0; pr = eval("proc { x = 5 }"); pr.call; p x
z = 1; pr3 = eval("proc { z }"); z = 7; p pr3.call
y = 0; Object.new.instance_eval("proc { y = 6 }").call; p y
a = 1; eval("a += 10"); p a
b = binding; b.eval("nw = 3"); p b.local_variable_get(:nw), b.eval("nw + 1"), b.local_variable_defined?(:nw)
def m(k) = eval("[k, proc { k += 1 }]")
v, inc = m(10); inc.call; p inc.call
c1 = 5; p eval("c1 * 2"), eval("binding").local_variable_get(:c1)
[1, 2].each { |i| p eval("i * 100") }
q = 1; p eval("eval('q + 1')"), eval("-> { q + 2 }").call
def n; w = 1; [1].map { |e| eval("w + e") }; end; p n
p (eval "defined?(zz)"), eval("zz = 1; zz"), (defined?(zz))
t = 3; b2 = binding; b2.local_variable_set(:t, 9); p t, eval("t", b2)
