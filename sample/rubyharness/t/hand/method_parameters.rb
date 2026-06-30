def foo(a, b=1, *c, d:, **e, &f); end
p method(:foo).parameters
def bar(x, y); end
p method(:bar).parameters
def baz(a, b=2, *rest); end
p method(:baz).parameters
def kw(name:, age: 0); end
p method(:kw).parameters
def blk(&block); end
p method(:blk).parameters
def noargs; end
p method(:noargs).parameters
class C; def m(a, b:); end; end
p C.new.method(:m).parameters
p C.instance_method(:m).parameters
def opt(a, b = 1, c = 2); end
p method(:opt).parameters
def post(a, *b, c); end
p method(:post).parameters
obj = Object.new
def obj.singleton_m(p, q); end
p obj.method(:singleton_m).parameters
def kwrest(a, **opts); end
p method(:kwrest).parameters
def allkinds(req, opt=1, *rest, key:, optkey: 2, **kwrest, &blk); end
p method(:allkinds).parameters
