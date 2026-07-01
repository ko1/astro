# Proc/lambda/Method #arity account for keyword arguments: required kw adds 1;
# a lambda/method negates for keyword optionality (only when no required kw), a
# plain proc never negates for keywords. vs ruby.
p ->(x, y:) { }.arity
p ->(x, y: 1) { }.arity
p ->(x, y:, z: 1) { }.arity
p ->(**k) { }.arity
p ->(x, **k) { }.arity
p ->(a, b, k1:, k2: 2, **r) { }.arity
p proc { |x, y:| }.arity
p proc { |x, y: 1| }.arity
p proc { |x, *y| }.arity
p ->(x, y) { }.arity
p ->(x, *y) { }.arity
def m1(a, b:); end
def m2(a, b: 1); end
def m3(a, b, c:, d: 1, **k); end
def m4(a, *r, k:); end
def m5(**k); end
p method(:m1).arity
p method(:m2).arity
p method(:m3).arity
p method(:m4).arity
p method(:m5).arity
