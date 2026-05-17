# nqueens — count solutions to the N-queens problem via backtracking.
#
# Macro benchmark for: deep recursion + per-frame small Array allocation
# (the "column set" passed down).  Backtracking has a tree-of-calls
# shape — each call may spawn N children, each creating a fresh Array
# of length up to N.  Lifetime is strictly LIFO and shallow, so this
# stresses the nursery survival/promotion threshold more than the
# remembered set.
#
# N=11 gives 2680 solutions and runs ~1-2 s in plain mode.

def safe(cols, row, col)
  i = 0
  n = cols.size
  while i < n
    c = cols[i]
    d = row - i
    if d < 0
      d = 0 - d
    end
    if c == col
      return false
    end
    if c - col == d
      return false
    end
    if col - c == d
      return false
    end
    i = i + 1
  end
  true
end

def solve(n, row, cols)
  if row == n
    return 1
  end
  count = 0
  col = 0
  while col < n
    if safe(cols, row, col)
      # Push col onto a fresh array (functional style — Array.dup is
      # not available so we rebuild).
      next_cols = []
      i = 0
      r = cols.size
      while i < r
        next_cols.push(cols[i])
        i = i + 1
      end
      next_cols.push(col)
      count = count + solve(n, row + 1, next_cols)
    end
    col = col + 1
  end
  count
end

p solve(11, 0, [])
