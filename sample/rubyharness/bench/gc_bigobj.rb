# Large-object churn: allocate big short-lived arrays that overflow a young
# nursery, forcing frequent collection of large live spans.  Contrasts a copying
# collector (must copy each surviving big array) with mark-sweep / immix (mark in
# place).  Most arrays die before the next collection, so it also measures bulk
# allocation throughput.  Deterministic (== CRuby).
BIG = 2_000
OUTER = 6_000

acc = 0
i = 0
while i < OUTER
  a = Array.new(BIG) { |j| j + i }        # large short-lived array
  acc += a[BIG - 1] + a[0]
  i += 1
end
p acc
