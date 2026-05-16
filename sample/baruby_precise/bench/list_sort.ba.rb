# list_sort — macro benchmark: build a random-ish array of integers,
# sort it with merge sort (recursive, allocates per merge), repeat.
#
# Merge sort is allocation-heavy: each level of recursion allocates two
# halves and a merged output, all dying when the parent merge returns.
# Plus the final sorted array stays live until verification.
#
# Pattern: deep recursion + bursts of medium-lived allocs that all die
# at one level's exit.  Good differentiator for nursery-vs-mark GCs.

def merge(a, b)
  ai = 0
  bi = 0
  r  = []
  na = a.size
  nb = b.size
  while ai < na
    if bi >= nb
      r.push(a[ai])
      ai = ai + 1
    else
      av = a[ai]
      bv = b[bi]
      if av <= bv
        r.push(av)
        ai = ai + 1
      else
        r.push(bv)
        bi = bi + 1
      end
    end
  end
  while bi < nb
    r.push(b[bi])
    bi = bi + 1
  end
  r
end

def msort(a)
  n = a.size
  if n <= 1
    a
  else
    mid = n / 2
    left  = []
    right = []
    i = 0
    while i < mid
      left.push(a[i])
      i = i + 1
    end
    while i < n
      right.push(a[i])
      i = i + 1
    end
    merge(msort(left), msort(right))
  end
end

# Build pseudo-random input.
def make_input(n)
  a = []
  x = 1
  i = 0
  while i < n
    x = (x * 1103515245 + 12345)
    # clamp to a reasonable positive range
    if x < 0
      x = 0 - x
    end
    a.push(x - (x / 1000000) * 1000000)
    i = i + 1
  end
  a
end

# Verify sorted (sum is invariant under sorting; also check monotonic).
def is_sorted(a)
  n = a.size
  i = 1
  ok = 1
  while i < n
    if a[i - 1] > a[i]
      ok = 0
    end
    i = i + 1
  end
  ok
end

# 2000-element input, 350 iterations = ~1s on copy backend.
acc = 0
iter = 0
while iter < 350
  a = make_input(2000)
  s = msort(a)
  acc = acc + is_sorted(s) + s[0]
  iter = iter + 1
end
p acc
