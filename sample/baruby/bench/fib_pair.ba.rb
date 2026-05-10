# Recursive Fibonacci variant that returns a 2-element pair on every
# call.  At depth N, the call tree allocates O(2^N) BaArrays, most of
# them dying as soon as their parent returns — pure frame-escape /
# short-lived alloc pattern with deep stack.

def fib_pair(n)
  if n < 2
    [n, n + 1]
  else
    a = fib_pair(n - 1)
    b = fib_pair(n - 2)
    [a[0] + b[0], a[1] + b[1]]
  end
end

# Depth 28: fib_pair tree allocates ~317k BaArrays per top-level call.
# Loop 13× to land at ~1 s wall.  Each leaf returns a fresh 2-element
# array, every inner node allocates one more — pure short-lived alloc
# with a deep call stack.
i   = 0
sum = 0
while i < 13
  r   = fib_pair(28)
  sum = sum + r[0] + r[1]
  i   = i + 1
end
p sum
