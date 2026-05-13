# Allocate a million arrays, sum the first element of each.
# Pure allocation pressure: each iteration allocates a 4-element
# array, drops it on the floor next iteration so the collector has
# to keep up.

def run(iters)
  sum = 0
  i = 0
  while i < iters
    a = [i, i+1, i+2, i+3]
    sum = sum + a[0] + a[3]
    i = i + 1
  end
  sum
end

p run(10_000_000)
