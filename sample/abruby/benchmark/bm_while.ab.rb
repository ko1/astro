# Simple while loop (measures loop + fixnum add overhead)
i = 0
while i < 300_000_000
  i += 1
end

p i
