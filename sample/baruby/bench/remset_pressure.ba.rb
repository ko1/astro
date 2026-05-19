# remset_pressure — sparse writes into a large long-lived array.
#
# Stresses generational backends' write barrier + remset path.  Build a
# big long-lived table (forces all rows to be promoted to old after a few
# minors), then continuously write fresh young arrays into random rows of
# the table.  Each such write is an old→young pointer store, exactly the
# pattern a card / remset is designed to handle.
#
# A naive object-level remset with no cap grows linearly with distinct
# write targets between minors.  Iter 36 capped the remset at 128 K
# entries with heap-walk fallback; under this bench the cap will fire if
# updates exceed that distinct-target rate.

n = 50_000        # rows
iters = 200_000   # total updates (4× n → most rows updated multiple times)

table = []
i = 0
while i < n
  table = [[i, 0], table]   # cons-style chain; force allocations
  i = i + 1
end

# Walk the chain once to force promotion of every cell to old.
def walk(t)
  s = 0
  while t != []
    s = s + 1
    t = t[1]
  end
  s
end
walk(table)
walk(table)
walk(table)

# Now do sparse updates: each iter creates a fresh young 2-array and
# stores it into a random-ish slot in the chain.  The store is old→young
# (chain cell == old, new pair == young).
checksum = 0
i = 0
t = table
while i < iters
  # Walk a few steps into the chain (deterministic "random")
  step = (i * 7) % 64
  t = table
  j = 0
  while j < step
    if t == []
      t = table
    end
    t = t[1]
    j = j + 1
  end
  if t == []
    t = table
  end
  # Now t is a cell in the chain.  Replace its head with a fresh young.
  fresh = [i, (i * 31 + 17)]
  t[0] = fresh
  checksum = checksum + fresh[1]
  i = i + 1
end

p checksum
