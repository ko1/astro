# While loop inside a method
def bench
  i = 0
  while i < 125_000
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
