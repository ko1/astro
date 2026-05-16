# life — Conway's Game of Life on a W×H grid for T ticks.
#
# Macro benchmark: each tick allocates a fresh outer Array of H rows,
# each row a fresh Array of W ints (0/1).  Previous tick's grid dies
# wholesale once the new one is built — a textbook nursery-friendly
# workload but with bigger per-tick alloc bursts than fib_pair etc.
#
# Stresses:
# - Allocator for mid-size Array bursts (W * H + H allocs per tick)
# - Tenuring threshold (grid lives just one tick — should never tenure)
# - Mark/sweep walk (dead-heavy: ~all of previous grid is dead each tick)
#
# NB: avoids `n + get(g, w, h, x', y')` style — baruby's parser bug
# makes calls with >3 args inside binop subexpressions clobber the
# operand slot.  Each call's result is bound to a local first.

def make_grid(w, h)
  g = []
  i = 0
  while i < h
    row = []
    j = 0
    while j < w
      # ~40% live, clustered.  A xor-fold of (i, j) gives clumps; flat
      # checkerboards die instantly because every cell has 4 live
      # neighbours (rule: 4+ → die).
      a1 = i * 1009
      a2 = j * 2027
      a3 = i * j * 41
      seed = (a1 + a2 + a3 + 7) % 5
      if seed < 2
        row.push(1)
      else
        row.push(0)
      end
      j = j + 1
    end
    g.push(row)
    i = i + 1
  end
  g
end

def get(g, w, h, x, y)
  if x < 0
    return 0
  end
  if y < 0
    return 0
  end
  if x >= w
    return 0
  end
  if y >= h
    return 0
  end
  row = g[y]
  row[x]
end

def step(g, w, h)
  ng = []
  y = 0
  while y < h
    row = []
    x = 0
    while x < w
      # Eight-neighbour sum.  Bind each call result to a temp first to
      # work around baruby parser quirk with >3-arg calls in binops.
      xm = x - 1
      yp = y - 1
      a = get(g, w, h, xm, yp)
      b = get(g, w, h, x,  yp)
      xp = x + 1
      c = get(g, w, h, xp, yp)
      d = get(g, w, h, xm, y)
      e = get(g, w, h, xp, y)
      yn = y + 1
      f = get(g, w, h, xm, yn)
      gg = get(g, w, h, x,  yn)
      hh = get(g, w, h, xp, yn)
      n = a + b
      n = n + c
      n = n + d
      n = n + e
      n = n + f
      n = n + gg
      n = n + hh
      cur = get(g, w, h, x, y)
      live = 0
      if cur == 1
        if n == 2
          live = 1
        end
        if n == 3
          live = 1
        end
      else
        if n == 3
          live = 1
        end
      end
      row.push(live)
      x = x + 1
    end
    ng.push(row)
    y = y + 1
  end
  ng
end

def population(g, w, h)
  c = 0
  y = 0
  while y < h
    row = g[y]
    x = 0
    while x < w
      cell = row[x]
      c = c + cell
      x = x + 1
    end
    y = y + 1
  end
  c
end

w = 80
h = 80
ticks = 200

g = make_grid(w, h)
t = 0
while t < ticks
  g = step(g, w, h)
  t = t + 1
end
p population(g, w, h)
