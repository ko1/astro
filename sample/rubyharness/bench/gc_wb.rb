# Write-barrier / remembered-set stress: a large long-lived array of mutable
# holders (ages into the old generation) whose slots are repeatedly overwritten
# to point at freshly-allocated young objects.  Each store creates an old->young
# edge, exercising the generational write barrier / remembered set.  A
# generational GC must track these edges without rescanning the whole old set on
# every minor; a non-generational GC pays a full mark/copy each time.
# Deterministic (== CRuby).
N = 30_000
ROUNDS = 60

holders = Array.new(N) { |i| [i, nil] }   # long-lived, retained for the whole run

acc = 0
r = 0
while r < ROUNDS
  k = 0
  while k < N
    holders[k][1] = [r, k, r + k]         # old holder now points at a fresh young array
    acc += holders[k][1][2] & 1
    k += 1
  end
  r += 1
end
acc += holders[N - 1][0]
p acc
