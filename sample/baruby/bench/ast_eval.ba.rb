# ast_eval — synthesizes a deep AST then evaluates it many times.
#
# Macro pattern: a builder phase (alloc-heavy, all promoted) followed by
# an evaluator phase that walks the long-lived AST and creates short-lived
# intermediate arrays during evaluation.  Combines:
#   - Long-lived AST (promoted to old in gen backends)
#   - Per-evaluation short-lived Array results
#   - Pointer chasing through nested arrays (cache-cold)
#
# Real-world analogue: an interpreter walking IR / bytecode many times.

# Build a tree-shaped AST.  Each node is [op, lhs, rhs] where:
#   op = 0: const → returns lhs (a number)
#   op = 1: add → lhs + rhs
#   op = 2: mul → lhs * rhs
#   op = 3: sub → lhs - rhs
#   op = 4: ite → if lhs != 0 then rhs[0] else rhs[1]
def make_tree(depth)
  if depth <= 0
    [0, depth + 1, 0]   # const(depth+1)
  else
    op = depth % 4 + 1     # 1..4 op
    if op == 4
      cond    = make_tree(depth - 1)
      then_br = make_tree(depth - 1)
      else_br = make_tree(depth - 1)
      [4, cond, [then_br, else_br]]
    else
      [op, make_tree(depth - 1), make_tree(depth - 1)]
    end
  end
end

def eval_tree(t)
  op = t[0]
  if op == 0
    t[1]
  elsif op == 1
    eval_tree(t[1]) + eval_tree(t[2])
  elsif op == 2
    eval_tree(t[1]) * eval_tree(t[2])
  elsif op == 3
    eval_tree(t[1]) - eval_tree(t[2])
  else
    cond = eval_tree(t[1])
    branches = t[2]
    if cond != 0
      eval_tree(branches[0])
    else
      eval_tree(branches[1])
    end
  end
end

# Depth 14 = ~32K nodes.  Evaluate 200 times for sustained scale.
tree = make_tree(14)

sum = 0
iter = 0
while iter < 200
  v = eval_tree(tree)
  # Mod to keep numbers bounded
  sum = sum + (v % 1000003)
  iter = iter + 1
end
p sum
