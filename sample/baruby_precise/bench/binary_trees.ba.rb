# Computer Language Benchmarks Game style binary-trees.
# A leaf is encoded as [0, 0]; an inner node as [left, right].
# `&&` is normalised to true/false in baruby so this works without nil.

def make_tree(depth)
  if depth <= 0
    [0, 0]
  else
    [make_tree(depth-1), make_tree(depth-1)]
  end
end

def check_tree(tree)
  l = tree[0]
  r = tree[1]
  if l == 0
    if r == 0
      1
    else
      1 + check_tree(l) + check_tree(r)
    end
  else
    1 + check_tree(l) + check_tree(r)
  end
end

# depth 21 = ~4M leaves; ~330MB through libgc, ~12 GCs, ~1s wall.
n = 21
sum = 0
i = 0
while i < 1
  sum = sum + check_tree(make_tree(n))
  i = i + 1
end
p sum
