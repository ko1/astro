# Method-wrapped while loop (measures loop + fixnum add overhead).
# Unlike loop.rb (one top-level loop), the hot loop lives in a method called
# many times, so a method-entry JIT (YJIT — no OSR) can actually compile it.
def bench
  i = 0
  while i < 40_000
    i += 1
  end
  i
end

result = 0
i = 0
while i < 1000
  result = bench
  i += 1
end
p(result)
