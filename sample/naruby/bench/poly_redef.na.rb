# Polymorphic redef bench: demonstrates the PGO win.
#
# Two defs of `f`:
#   - body_v1 (`x + 999`) — declared first, **never called**
#   - body_v2 (`x + 1`)   — declared second, called 5×10⁷ times in the loop
#
# Last-def-wins (= first-def-wins in current code_repo: linear search
# returns the first matching name): parse-time picks body_v1 as
# `sp_body` for the call site.  At runtime cc holds body_v2 (the
# later def is what `fe->body` ends up at).  cc->body != sp_body, so
# the 2-step guard fails every iteration → slowpath fires 5×10⁷ times.
#
# Profile-derived speculation: profile from prior run records 5×10⁷
# observations of body_v2, 0 of body_v1.  Parser picks body_v2 as
# sp_body.  cc->body == sp_body → fast path hits.  Expected speedup
# vs without profile: ~5–10× (chain-of-1 call, but every call avoids
# slowpath).

def f(x)
  x + 999
end

def f(x)
  x + 1
end

i = 0
acc = 0
while i < 50000000
  acc = acc + f(10)
  i = i + 1
end
p(acc)
