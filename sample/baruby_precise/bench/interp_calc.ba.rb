# interp_calc — macro benchmark: build & walk small arithmetic ASTs.
#
# Each "expression" is an N-deep balanced tree of arrays:
#   [kind, lhs, rhs]   with kind = 0 (num leaf) | 1 (add) | 2 (sub)
# Leaves: [0, value, 0].
#
# Build phase allocates O(2^depth) short-lived sub-arrays; walk allocs
# nothing (just int arithmetic).  Half the run hammers the allocator,
# the other half stresses recursion + array indexing.

def make_expr(depth, seed)
  if depth <= 0
    [0, seed, 0]
  else
    if depth % 2 == 1
      [1, make_expr(depth - 1, seed), make_expr(depth - 1, seed + 1)]
    else
      [2, make_expr(depth - 1, seed + 2), make_expr(depth - 1, seed)]
    end
  end
end

def eval_expr(e)
  k = e[0]
  if k == 0
    e[1]
  else
    l = eval_expr(e[1])
    r = eval_expr(e[2])
    if k == 1
      l + r
    else
      l - r
    end
  end
end

# Depth 12 = ~4k inner nodes per AST.  Loop 1000x to hit ~1s.
# XOR rather than sum so a regression that drops one tree's result still
# changes the answer (sum can hide regressions when results cancel).
acc = 0
i = 0
while i < 1000
  e = make_expr(12, i + 1)
  acc = acc + eval_expr(e) + i
  i = i + 1
end
p acc
