i = 0
begin
  i += 1
end while i < 5
p i
# body runs once even if cond false
j = 10
begin
  j += 1
end while j < 5
p j
# begin/end until
k = 0
begin
  k += 1
end until k >= 3
p k
# with break
m = 0
begin
  m += 1
  break if m == 2
end while true
p m
