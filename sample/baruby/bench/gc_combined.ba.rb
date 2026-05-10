# Mixed lifetimes: a 50k-element long-lived array sits in scope while
# the inner loop churns 1M short-lived 4-element arrays.  Models
# steady-state workloads that have a permanent dataset plus a hot
# allocation path on top.
#
# Boehm GC is non-generational so the "young space promotion" benefit
# doesn't materialise here, but the bench is useful as a baseline for
# when we swap in a precise GC that does have a nursery.

def fill(n)
  a = []
  i = 0
  while i < n
    a << i
    i = i + 1
  end
  a
end

def churn(long, iters)
  sum = 0
  i = 0
  while i < iters
    short = [i, i + 1, i + 2, i + 3]
    sum = sum + short[0] + long[i % 1000]
    i = i + 1
  end
  sum
end

long = fill(50_000)
p churn(long, 10_000_000)
