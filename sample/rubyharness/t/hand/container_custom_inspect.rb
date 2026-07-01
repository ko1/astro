# A user object's custom #inspect is honoured when it appears inside a container
# (Array/Hash/Set/nested), via p / #inspect / #to_s / string interpolation. vs ruby.
class Point
  def initialize(x, y); @x, @y = x, y; end
  def inspect; "#<Point (#{@x},#{@y})>"; end
  def to_s; "Point(#{@x},#{@y})"; end
end
pt = Point.new(1, 2)
p pt
p [pt]
p [pt, pt]
p({ a: pt })
p({ pt => 1 })
p [[pt]]
p [1, pt, "s", :sym, nil]
puts [pt].inspect
puts [pt].to_s
puts({ k: pt }.to_s)
p "arr: #{[pt]}"
puts "arr: #{[pt]}"
p "hash: #{{a: pt}}"
p({ deep: { arr: [pt] } })
# nested with two custom classes
class Money; def initialize(c); @c = c; end; def inspect; "$#{@c}"; end; end
p [Money.new(5), { price: Money.new(10) }, [Money.new(15)]]
# plain object (no custom inspect) unchanged
class Plain; end
p [Plain.new].map { |o| o.inspect.start_with?("#<Plain") }
# non-custom containers unchanged
p [1, [2, 3], { a: 1 }]
p "x #{[1, 2]} y"
