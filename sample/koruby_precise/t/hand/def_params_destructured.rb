# Post parameters that sit behind a destructuring one, and the anonymous `*`
# whose reserved slot has no name.  Both used to abort at parse time.

# a destructured post takes one staged slot but two locals, so `d` lands one
# slot below its own local and has to be moved before the unpack runs.
def d4o(a = 1, (b, c), d, &e); [a, b, c, d]; end
def d5o(a = 1, f = 2, (b, c), d, &e); [a, f, b, c, d]; end
def d7o(a = 1, f = 2, (b, c), (d, e), &g); [a, f, b, c, d, e]; end
p d4o([2, 3], 4)              # [1, 2, 3, 4]
p d4o(9, [2, 3], 4)           # [9, 2, 3, 4]
p d5o([2, 3], 4)              # [1, 2, 2, 3, 4]
p d5o(9, 8, [2, 3], 4)        # [9, 8, 2, 3, 4]
p d7o([2, 3], [4, 5])         # [1, 2, 2, 3, 4, 5]

def p1((a, b), c); [a, b, c]; end
def p2(x, (a, b), c); [x, a, b, c]; end
def p3(*r, (a, b), c); [r, a, b, c]; end
def p4((a, b), (c, d), e); [a, b, c, d, e]; end
p p1([1, 2], 3)               # [1, 2, 3]
p p2(0, [1, 2], 3)            # [0, 1, 2, 3]
p p3(9, 9, [1, 2], 3)         # [[9, 9], 1, 2, 3]
p p4([1, 2], [3, 4], 5)       # [1, 2, 3, 4, 5]

# nested / splat inside the destructured post
def n1(a = 1, (b, (c, d)), e); [a, b, c, d, e]; end
def n2(a = 1, (b, *c), d); [a, b, c, d]; end
p n1([2, [3, 4]], 5)          # [1, 2, 3, 4, 5]
p n2([2, 3, 4], 5)            # [1, 2, [3, 4], 5]
p n2([2], 5)                  # [1, 2, [], 5]

# short/long arrays pad and truncate like a masgn
def s1((a, b, c), d); [a, b, c, d]; end
p s1([1], 2)                  # [1, nil, nil, 2]
p s1([1, 2, 3, 4], 5)         # [1, 2, 3, 5]

# anonymous rest followed by a post: the reserved slot has no name, so it must
# stay out of local_variables (and out of the constant pool lookup).
def ar(*, a); a; end
def ar2(*, a, b); [a, b]; end
def arv(*, a); [a, local_variables]; end
p ar(1)                       # 1
p ar(1, 2, 3)                 # 3
p ar2(1, 2, 3)                # [2, 3]
p arv(1, 2)                   # [2, [:a]]
p method(:ar).parameters      # [[:rest, :*], [:req, :a]]
p method(:ar).arity           # -2
p method(:d4o).parameters
p method(:d4o).arity          # -3

# `_` may still repeat (a non-`_` param AFTER a repeated `_` is still unsupported)
def rep(_, _); 3; end
p rep(1, 2)                   # 3
