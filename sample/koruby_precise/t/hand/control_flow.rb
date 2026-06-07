# L0: if / unless / while / until / case / loop control (no blocks/methods)
x = 5
if x > 3
  p "big"
else
  p "small"
end

p (x > 3 ? "yes" : "no")

if x < 0
  p "neg"
elsif x == 0
  p "zero"
else
  p "pos"
end

p "positive" if x > 0
p "nonzero" unless x == 0

unless x > 10
  p "not huge"
end

i = 0
while i < 5
  print i
  i += 1
end
puts

j = 10
until j <= 5
  print j
  j -= 1
end
puts

k = 0
loop do
  k += 1
  break if k >= 3
end
p k

sum = 0
n = 1
while n <= 10
  n += 1
  next if n.even?
  sum += n
end
p sum

total = 0
m = 0
while true
  m += 1
  break if m > 100
  next unless m % 10 == 0
  total += m
end
p total

case x
when 0 then p "zero"
when 1..4 then p "low"
when 5..9 then p "mid"
else p "high"
end

grade = case 85
        when 90..100 then "A"
        when 80..89  then "B"
        else "C"
        end
p grade

case "hello"
when String then p "is string"
when Integer then p "is int"
end

v = 3
r = if v > 2 then "gt" else "le" end
p r

p(begin; 1 + 1; end)
