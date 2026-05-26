# hash_chain — macro benchmark: build a chained-bucket hash table on top
# of Array (baruby has no Hash), insert lots of keys, then look them up.
#
# History: this bench surfaced a stale-ptr bug in aro_gc_realloc_payload
# (memcpy-to-buf-before-alloc captured pre-GC ptr values; the alloc's GC
# moved their targets, leaving stale entries in the new payload).  Fixed
# in gc_copy_gen.c / gc_mark_compact_gen.c by allocating
# first then memcpy'ing from the post-GC (forwarded) old location via
# oldh->fwd.  Now passes on all 10 backends.
#
# Profile:
# - Long-lived: the 2048-bucket Array
# - Medium-lived: per-bucket chain Arrays (one per non-empty bucket)
# - Short-lived: 2-element [key, value] pairs allocated on every insert
#
# Stresses:
# - Allocator for many small Arrays (the [k, v] pairs)
# - WB heavily — every chain.push writes a heap pointer into a long-lived
#   bucket array slot, exercising the old→young remset for gen backends
# - Array.push growth (resize → realloc payload, also a WB-relevant write)
# - Read-side: scan each chain on every lookup

def make_table(n)
  t = []
  i = 0
  while i < n
    t.push([])
    i = i + 1
  end
  t
end

def hash_key(k)
  # Multiplicative hash; /16 substitutes for >>4 (baruby has no shift).
  (k * 2654435761) / 16
end

def insert(t, k, v)
  b = hash_key(k) % 2048
  chain = t[b]
  i = 0
  n = chain.size
  while i < n
    e = chain[i]
    if e[0] == k
      e[1] = v
      return
    end
    i = i + 1
  end
  chain.push([k, v])
end

def lookup(t, k)
  b = hash_key(k) % 2048
  chain = t[b]
  i = 0
  n = chain.size
  while i < n
    e = chain[i]
    if e[0] == k
      return e[1]
    end
    i = i + 1
  end
  -1
end

t = make_table(2048)

# Insert 150k keys, repeat 3 times so steady-state allocs dominate setup.
rounds = 3
keys   = 150_000
r = 0
while r < rounds
  i = 0
  while i < keys
    insert(t, i, i * 3 + r)
    i = i + 1
  end
  r = r + 1
end

sum = 0
i = 0
while i < keys
  v = lookup(t, i)
  sum = sum + v
  i = i + 1
end
p sum
