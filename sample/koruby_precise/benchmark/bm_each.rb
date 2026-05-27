# Array#each with a minimal block body — measures Array iterator + yield
# overhead.
def bench(ary)
  sum = 0
  ary.each { |x| sum += x }
  sum
end

ary = []
i = 0
while i < 40_000
  ary.push(i)
  i += 1
end

result = 0
j = 0
while j < 1000
  result = bench(ary)
  j += 1
end
p(result)
