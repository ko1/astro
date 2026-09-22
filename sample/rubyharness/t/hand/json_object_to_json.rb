# The json gem's default Object#to_json is the object's #to_s as a JSON string
# (generating self would come straight back into the generator).
require "json"

class Point
  def initialize(x, y) = (@x, @y = x, y)
  def to_s = "(#{@x},#{@y})"
end

class Tagged
  def to_json(*) = '{"tag":"t"}'
end

p Point.new(1, 2).to_json
p JSON.generate(Point.new(1, 2))
p JSON.generate([Point.new(1, 2), Tagged.new])
p JSON.generate({p: Point.new(3, 4)})
p RuntimeError.new("boom").to_json
p (1..3).to_json
p JSON.generate("plain")
p JSON.generate([1, "two", :three, nil, true, 1.5])
