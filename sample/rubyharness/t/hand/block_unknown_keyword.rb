# Unknown keywords raise for blocks, procs, lambdas and define_method bodies too,
# after the missing-keyword check and before any default is evaluated.
def t; yield; rescue ArgumentError => e; p e.message; else p :ok; end
t { proc { |name:| name }.call }
t { ->(name:) { name }.call }
t { [1].each { |x, k: 1| } }
def y; yield(1, z: 2); end
t { y { |x, k: 1| } }
t { y { |x, **o| p o } }
def y(*a, **k) = yield(*a, **k)
t { y(1, z: 2) { |x, k: (p :default; 1)| } }
t { y(1, z: 2, w: 3) { |x, k:| } }
t { y(1, z: 2) { |x, k: 1| } }
t { y(1, "s" => 2) { |x, k: 1| } }
t { y({z: 2}) { |x, k: 1| p x } }
t { y(1, k: 5) { |x, k: 1| p k } }
t { [[1, {z: 1}]].each { |a, k: 1| p a } }
h = {z: 1}; t { y(1, **h) { |x, k: 1| } }
t { y(1) { |x, k: 1| p k } }
k = Class.new { define_method(:a) { |name:| name }; define_method(:b) { |x| x }; define_method(:c) { |x, y = 1, *r, k: 2, **o| [x, y, r, k, o] }; define_method(:d) { nil } }.new
t { k.a(name: 1, url: 2) }
t { k.a }
t { k.a(name: 1) }
t { k.b(1, 2) }
t { k.b }
t { k.d(1) }
t { k.d(u: 1) }
p k.c(1, 2, 3, k: 4, z: 5)
l = ->(name:) { name }; t { l.call(name: 1, url: 2) }
pr = proc { |name:| name }; t { pr.call(name: 1, url: 2) }
m = k.method(:a); t { m.call(name: 1, url: 2) }
p k.method(:a).arity, k.method(:c).arity, k.method(:a).parameters
