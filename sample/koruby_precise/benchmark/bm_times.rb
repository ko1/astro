# Integer#times with a minimal block body — measures yield / block dispatch
# overhead relative to while-loop baseline (bm_while).
def bench
  sum = 0
  40_000.times { |i| sum += i }
  sum
end

result = 0
i = 0
while i < 1000
  result = bench
  i += 1
end
p(result)
