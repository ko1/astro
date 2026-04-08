def f
  i = 0
  while i < 300_000
    i += 1
  end
end

i = 0
while i < 1_000
  f
  i += 1
end
