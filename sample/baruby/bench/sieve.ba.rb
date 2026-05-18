# sieve — Sieve of Eratosthenes for primes up to N.
#
# Allocates one big boolean-ish array (true/false markers), sweeps it
# to cross out composites, then collects survivors into a primes Array.
# Long-lived sieve array dominates the heap for the entire run; result
# array also long-lived (returned to caller).
#
# Differentiator: O(N) linear walk over a large boolean array, with
# scattered writes (composite cross-off pattern j += i).  Stresses:
# - Allocator for a single large payload (the sieve array)
# - WB-heavy write path: every s[j] = false is a heap-pointer write
#   (false is a VAL singleton, but baruby_gc_wb still runs)
# - Cache locality on linear sweep; scattered cross-offs hit different
#   pages
#
# N=10_000_000 yields 664579 primes, runs ~0.8-2 s depending on backend.

def sieve(n)
  s = []
  i = 0
  while i <= n
    s.push(true)
    i = i + 1
  end
  s[0] = false
  s[1] = false
  i = 2
  while i * i <= n
    if s[i]
      j = i * i
      while j <= n
        s[j] = false
        j = j + i
      end
    end
    i = i + 1
  end
  result = []
  i = 2
  while i <= n
    if s[i]
      result.push(i)
    end
    i = i + 1
  end
  result
end

primes = sieve(10_000_000)
p primes.size
