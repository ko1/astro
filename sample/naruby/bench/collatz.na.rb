# Collatz step count: how many steps for `n` to reach 1.
# Mixes if/else inside while; an even/odd dispatch every iter.
def collatz_steps(n)
  steps = 0
  while n != 1
    if n % 2 == 0
      n = n / 2
    else
      n = n * 3 + 1
    end
    steps += 1
  end
  steps
end

# Sum collatz_steps over a range — sustained ~1 sec workload.
sum = 0
i = 1
while i < 1_000_000
  sum += collatz_steps(i)
  i += 1
end
p sum
