# Branchy call: cond is constant true at runtime.  PGSD speculation
# should bake against the dominant branch.
def f(n)
  if n < 1000000000
    n + 1
  else
    n * 2
  end
end

i=0
while i<50_000_000
  f(42)
  i += 1
end
p f(42)
