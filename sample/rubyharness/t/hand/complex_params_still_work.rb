# complex (non-repeated) parameter lists still work after the repeated-name
# soft-fail change. vs ruby.
def m(a, b, c = 1, *rest, k:, kk: 2, **kw, &blk); [a, b, c, rest, k, kk, kw, blk.nil?]; end
p m(1, 2, k: 9)
p m(1, 2, 3, 4, 5, k: 9, kk: 8, extra: 1)
def post(a, *b, c); [a, b, c]; end
p post(1, 2, 3, 4)
def opt(a, b = 10, c = 20); [a, b, c]; end
p opt(1)
p opt(1, 2, 3)
