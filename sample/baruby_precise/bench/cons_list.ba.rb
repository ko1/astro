# cons_list — macro benchmark: build & walk a singly-linked list of cons cells.
#
# Each cell is a 2-element array [value, next-cell].  We build a list of
# N cells then walk it summing values.  Repeat M times.
#
# Pattern: deep chain of small allocations (vs balanced tree of binary_trees).
# The walk is iterative so it doesn't blow the C stack like binary_trees does.

def build(n)
  list = 0   # 0 == nil-equivalent sentinel
  i = 0
  while i < n
    list = [i, list]
    i = i + 1
  end
  list
end

def walk(list)
  s = 0
  while list != 0
    s = s + list[0]
    list = list[1]
  end
  s
end

# 5000-cell list, 2000 iterations → ~1 s total on copy backend.
sum = 0
iter = 0
while iter < 2000
  l = build(5000)
  sum = sum + walk(l)
  iter = iter + 1
end
p sum
