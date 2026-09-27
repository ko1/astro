# Argument errors keep CRuby's messages after the message buffers moved out of the
# argument-binding frames (arity with optional / rest / keywords, unknown keywords).
def t = (yield rescue [$!.class, $!.message])
def a(x, y = 1) = x
def b(x, *r) = x
def c(x, k:, j: 2) = x
def d(x, y = 1, k: 1) = x
def e(k: 1) = k
def f(a, b = 1, *c, d, e:, **kw) = a
p t { a }, t { a(1, 2, 3) }, t { b }, t { c(1) }, t { c(1, k: 1, z: 2) }, t { c(1, k: 1, z: 2, w: 3) }, t { c }
p t { d(1, 2, 3) }, t { d(1, z: 1) }, t { d(1, 2, k: 3, q: 1) }, t { e(q: 1) }, t { e(1) }, t { e(k: 2, a: 1, b: 2) }
p t { f }, t { f(1) }, t { f(1, 2, e: 1) }, t { f(1, 2, 3, 4, e: 1, z: 9) }
h = { k: 1, z: 2 }; p t { c(1, **h) }, t { e(**h) }
