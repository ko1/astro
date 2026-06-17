Point = Struct.new(:x, :y)
pts = []
50.times { |i| pts << Point.new(i, i * 2) }
s = 0; j = 0
while j < 200_000
  pts.each { |p| s += p.x + p.y }
  j += 1
end
p s
