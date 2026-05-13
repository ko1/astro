# Concatenate many small strings.  Stresses the string allocator —
# every `+` produces a fresh BaString, and the literal "abc" itself
# allocates a fresh string each eval (no intern pool yet).

def run(iters)
  i = 0
  total = 0
  while i < iters
    s = "abc" + "def" + "ghi"
    total = total + s.size
    i = i + 1
  end
  total
end

p run(5_000_000)
