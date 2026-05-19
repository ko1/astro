# dll_walk — doubly-linked list build + forward + backward walk.
#
# Each node = [val, prev, next] (3-element array).  Different from
# cons_list (single-linked, 2-element) in:
#   - 2 outgoing pointers per node → mark phase touches 2× refs
#   - WB on `cur[2] = nxt` (old→young store as list grows in a single
#     long iter; remset gets exercised on gen backends)
#   - 3-element node alloc (vs cons_list's 2-element) — slightly bigger
#     alloc footprint, more byte alignment
#
# Iter pattern: build → walk forward → walk backward → drop list.
# Sentinel 0 == nil-equivalent.

def build_dll(n)
  head = [0, 0, 0]
  cur = head
  i = 1
  while i < n
    nxt = [i, cur, 0]
    cur[2] = nxt
    cur = nxt
    i = i + 1
  end
  [head, cur]
end

def walk_fwd(head)
  s = 0
  cur = head
  while cur != 0
    s = s + cur[0]
    cur = cur[2]
  end
  s
end

def walk_bwd(tail)
  s = 0
  cur = tail
  while cur != 0
    s = s + cur[0]
    cur = cur[1]
  end
  s
end

# 4000 cells × 1500 iters × 2 walks ≈ ~1 s on copy backend.
# Oracle: 2 × sum(0..3999) × 1500 = 2 × 7998000 × 1500 = 23994000000
sum = 0
iter = 0
while iter < 1500
  pair = build_dll(4000)
  head = pair[0]
  tail = pair[1]
  sum = sum + walk_fwd(head)
  sum = sum + walk_bwd(tail)
  iter = iter + 1
end
p sum
