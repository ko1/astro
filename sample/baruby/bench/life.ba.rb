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
      # Eight-neighbour sum, inline.  Parser handles >3-arg calls in
      # binop arg position via 4-slot reserve in alloc_binop (fix in
      # commit landed alongside this bench).
      n = get(g, w, h, x - 1, y - 1)
      n = n + get(g, w, h, x,     y - 1)
      n = n + get(g, w, h, x + 1, y - 1)
      n = n + get(g, w, h, x - 1, y)
      n = n + get(g, w, h, x + 1, y)
      n = n + get(g, w, h, x - 1, y + 1)
      n = n + get(g, w, h, x,     y + 1)
      n = n + get(g, w, h, x + 1, y + 1)
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
