# Block params: destructuring with optionals / posts (the general binder, logical arity), repeated `_` params (2026-10-16)
a = lambda { |(a, (*b, c)), d=1| [a, b, c, d] }
p a.arity, a.parameters
p a.call([1, [2, 3, 4]]), a.call([1, [2, 3]], 9)
b = proc { |a, (*b, c), d, (*e), (*), **k| [a, b, c, d, e, k] }
p b.arity, b.parameters
p b.call(1, [2, 3], 4, [5], [6], x: 1)
c = proc { |(a, (*b, c)), d=1| [a, b, c, d] }
p c.arity, c.call([1, [2, 3]]), c.call([[1, [2, 3]], 5])
e = proc { |x, (y, z), w = 7, *r, q| [x, y, z, w, r, q] }
p e.arity, e.call(1, [2, 3]), e.call(1, [2, 3], 4, 5, 6, 8)
f = lambda { |(a, b), c = 2, *r| [a, b, c, r] }
p f.arity, f.call([1, 9]), f.call([1, 9], 3, 4)
l1 = lambda { |x, (y, z), w = 7, *r, q| [x, y, z, w, r, q] }
p l1.arity, l1.call(1, [2, 3], 4), (l1.call(1) rescue $!.message), l1.call(1, [2, 3], 4, 5, 6)
l2 = lambda { |(a, b), *r| [a, b, r] }
p l2.arity, l2.call([1, 2]), l2.call([1, 2], 3), (l2.call rescue $!.message)
p3 = proc { |(a, b), *r, z| [a, b, r, z] }
p p3.call([1, 2]), p3.call([1, 2], 3), p3.call([1, 2], 3, 4)
l3 = lambda { |(a, b), c = 5| [a, b, c] }
p (l3.call([1,2], 3, 4) rescue $!.message), (l3.call rescue $!.message)
pr = proc {|_, _, _ = 1, *_, _:, _: 2, **_, &_| }
p pr.parameters, pr.arity
q = proc { |_, _| _ }
p q.call(1, 2), q.parameters
r = proc { |_a, _a, b| [_a, b] }
p r.call(1, 2, 3)
l = lambda { |_, _, *_, _k: 1| _ }
p l.call(5, 6, 7), l.arity
p [[1, 2], [3, 4]].map { |_, _| _ }
s = proc { |_, (x, y), _| [_, x, y] }
p s.call(1, [2, 3], 4) rescue p $!
