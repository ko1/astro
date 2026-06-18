INNER = 100_000
OUTER = 80

def bench
  keep = nil; local_i = 0
  while local_i < INNER
    a = [local_i, local_i + 1, local_i + 2]
    keep = a if (local_i & 65_535) == 0
    local_i += 1
  end
  keep
end

result = nil
i = 0
while i < OUTER
  result = bench
  i += 1
end
p result
