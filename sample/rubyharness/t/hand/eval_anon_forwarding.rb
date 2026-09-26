# An eval'd string forwards the enclosing method's anonymous *, **, & and ...
# (also through a block, a nested eval, a binding, and in a class method), and
# super(args) in it finds the method the binding was taken in.
def f(*a, **k, &b) = [a, k, b&.call]
def rest(*) = eval("f(*)")
def kw(**) = eval("f(**)")
def blk(&) = eval("f(&)")
def dots(...) = eval("f(...)")
p rest(1, 2), kw(a: 1), blk { :b }, dots(3, x: 4) { :d }
def in_block(*) = [1].map { eval("f(*)") }.first
p in_block(5, 6)
def nested(*) = eval("eval('f(*)')")
p nested(7)
def bind(*) = binding
p bind(8, 9).eval("f(*)"), bind(1).local_variables
class P; def m(*a) = [:p, a]; end
class Q < P; def m(*) = eval("super(*)"); end
p Q.new.m(1, 2)
def mix(a, *) = eval("[a, f(*)]")
p mix(0, 1, 2)
class P; def m(*a) = [:p, a]; end
class Q2 < P; def m(*) = eval("p([*]); super(*)"); end
p Q2.new.m(1, 2)
class Q3 < P; def m(*) = eval("super(*, 3)"); end
p Q3.new.m(1, 2)
class C; def r(*) = eval("[*]"); def r2(*) = eval("f(*)"); def f(*a) = a; end
p C.new.r(1, 2), C.new.r2(3)
def tr(*) = eval("[*]")
p tr(4, 5)
def tb(*) = binding.eval("[*]")
p tb(6)
