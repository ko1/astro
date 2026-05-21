# Bench specifically targeting `return` inside `if` (the new feature).
#
# `find_first_factor` returns the smallest factor > 1, or n itself when
# n is prime — using `return` inside an if to break out of the loop
# early.  Repeating across a wide range exercises both early-return
# (composite cases) and full-loop fallthrough (prime cases).

def find_first_factor(n)
  i = 2
  while i * i <= n
    if n % i == 0
      return i
    end
    i += 1
  end
  return n
end

# Sustained scale: ~1 sec.
sum = 0
n = 2
while n < 2_000_000
  sum += find_first_factor(n)
  n += 1
end
p sum
