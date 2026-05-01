# Array#map with a minimal block body — measures map allocation plus
# per-element yield.
def bench(ary)
  ary.map { |x| x * 2 }
end

ary = []
i = 0
while i < 5_000
  ary.push(i)
  i += 1
end

result = nil
j = 0
while j < 300
  result = bench(ary)
  j += 1
end
p(result.length)
