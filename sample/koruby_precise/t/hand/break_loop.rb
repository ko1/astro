# while-loop break
i = 0
while true
  i += 1
  break if i >= 5
end
p i
# break with value from while (result is nil for while, but value via assignment)
n = 0
while true
  n += 1
  break if n == 3
end
p n
# break in a block → each returns break value
r = [1,2,3,4].each { |x| break x*10 if x == 3 }
p r
# break with no value in block
s = [1,2,3].each { |x| break if x == 2; }
p s
# find-like via break
found = nil
[5,6,7].each { |x| if x.even?; found = x; break; end }
p found
# nested: while inside block
res = [1,2].map { |x| j=0; while true; j+=1; break if j>=x; end; j }
p res
