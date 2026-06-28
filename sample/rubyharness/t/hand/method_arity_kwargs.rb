def m1(a:); end; p method(:m1).arity
def m2(a: 1); end; p method(:m2).arity
def m3(a, b:); end; p method(:m3).arity
def m4(a, b, c:, d: 1); end; p method(:m4).arity
def m5(**k); end; p method(:m5).arity
def m6(a, **k); end; p method(:m6).arity
def m7(a, b: 1); end; p method(:m7).arity
def m8(a=1, b:); end; p method(:m8).arity
def m9(*a, b:); end; p method(:m9).arity
p method(:m4).unbind.arity
