# Hand-rolled inject using Array#each — exercises closure access to outer
# locals from inside a block (sum is defined in the enclosing method and
# read/written from the block body).
def inject_sum(ary)
  sum = 0
  ary.each { |x| sum += x }
  sum
end

ary = []
i = 0
while i < 20_000
  ary.push(i)
  i += 1
end

result = 0
j = 0
while j < 1000
  result = inject_sum(ary)
  j += 1
end
p(result)
