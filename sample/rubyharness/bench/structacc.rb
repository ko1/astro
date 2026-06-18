Point = Struct.new(:x, :y)
PTS = []
50.times { |i| PTS << Point.new(i, i * 2) }
INNER = 1_000
OUTER = 200

def bench
  s = 0; j = 0
  while j < INNER
    PTS.each { |p| s += p.x + p.y }
    j += 1
  end
  s
end

result = 0
i = 0
while i < OUTER
  result = bench
  i += 1
end
p(result)
