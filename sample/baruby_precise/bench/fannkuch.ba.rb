# fannkuch_redux — Computer Language Benchmarks Game classic.
#
# Compute max "flip count" over all permutations of 1..N.  Each "flip"
# reverses the prefix [0, p[0]-1] (so the value at p[0] heads to its
# correct position).  We count flips per permutation until p[0]==1.
#
# Pattern: tight loop of millions of permutations, each followed by a
# small-array allocation (working-copy w[]) and a write-heavy inner
# flip loop.  Two long-lived arrays (p[], count[]) drive permutation
# enumeration via incremental rotation (no recursion).  The dominating
# allocation is one ~N-element array per permutation; everything else
# is mutator-bound integer work.
#
# Differentiator: stresses fast nursery alloc + fast nursery promotion
# (each w[] dies within a few hundred mutator ops).  Non-generational
# backends pay for full mark+sweep over a heap dominated by already-dead
# w[]s; generational backends evacuate quickly.
#
# Enumeration is the canonical CLBG rotation-of-prefix algorithm: at
# each level r (0-indexed), rotate p[0..r] left by one r+1 times before
# moving up to r+1.

def fannkuch(n)
  p     = []
  count = []
  i = 0
  while i < n
    p.push(i + 1)
    count.push(0)
    i = i + 1
  end

  max_flips = 0
  r = n
  outer_done = false

  while outer_done == false
    # Reset rotation counters for levels we just exhausted on the way up.
    while r != 1
      count[r - 1] = r
      r = r - 1
    end

    # Skip identity (p[0]==1 gives 0 flips trivially).
    if p[0] != 1
      w = []
      i = 0
      while i < n
        w.push(p[i])
        i = i + 1
      end
      f = 0
      while w[0] != 1
        k = w[0]
        i = 0
        j = k - 1
        while i < j
          t = w[i]
          w[i] = w[j]
          w[j] = t
          i = i + 1
          j = j - 1
        end
        f = f + 1
      end
      if f > max_flips
        max_flips = f
      end
    end

    # Generate next permutation via rotation; exhaust levels upward.
    inner_done = false
    while inner_done == false && outer_done == false
      if r == n
        outer_done = true
      else
        first = p[0]
        i = 0
        while i < r
          p[i] = p[i + 1]
          i = i + 1
        end
        p[r] = first
        count[r] = count[r] - 1
        if count[r] > 0
          inner_done = true
        else
          r = r + 1
        end
      end
    end
  end
  max_flips
end

p fannkuch(9)
