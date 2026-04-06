# Collatz conjecture: count total steps for 1..N
def collatz_steps(n)
  steps = 0
  while n != 1
    if n % 2 == 0
      n = n / 2
    else
      n = 3 * n + 1
    end
    steps += 1
  end
  steps
end

total = 0
i = 1
while i <= 100000
  total += collatz_steps(i)
  i += 1
end
p(total)
