# Generational-GC workload: a large long-lived set (retained for the whole run,
# so it ages into the old generation) plus heavy short-lived allocation per call
# (dies young).  A generational collector minor-collects the young garbage
# without repeatedly copying/scanning the retained set, so it should beat a
# non-generational collector here.  Output is deterministic (== CRuby).
RETAINED = 40_000

def churn(n)
  acc = 0
  i = 0
  while i < n
    a = [i, i * 2, i * 3]                 # short-lived array
    h = { x: a[0], y: a[1], z: a[2] }     # short-lived hash
    s = "row-#{i & 63}"                   # short-lived string
    acc += h[:x] + h[:y] + h[:z] + s.length
    i += 1
  end
  acc
end

OUTER = 50
INNER = 20_000

# long-lived: built once, kept alive past the loop (referenced below) → old gen
retained = Array.new(RETAINED) { |i| { id: i, name: "obj#{i & 255}", tags: [i, i + 1] } }

result = 0
j = 0
while j < OUTER
  result += churn(INNER)
  j += 1
end
result += retained.length
p result
