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

# the natively-written types keep their own #to_json
p({"a" => 1, "b" => [1, 2, nil, true]}.to_json)
p [1, "x", :y].to_json
p [5.to_json, nil.to_json, true.to_json, "s".to_json, :sym.to_json, 1.5.to_json]
p({k: Point.new(1, 2)}.to_json)

# JSON::Coder — a generator/parser pair with its own callbacks
coder = JSON::Coder.new { |object, as_key| as_key ? "K:#{object}" : object.to_s }
p coder.dump({a: Point.new(1, 2)})
p coder.dump({Point.new(3, 4) => 1})
p coder.dump([Point.new(1, 2), 1, "s", nil])
p coder.load("[1,2]")
p JSON::Coder.instance_methods(false).sort
p JSON::Coder.new.dump("str")
p [JSON::Coder.new.dump([1, 2]), JSON::Coder.new.parse('{"a":1}')]

# a tagged Hash may hold more objects the generator does not know
tagger = JSON::Coder.new { |o, _| o.is_a?(Point) ? {"t" => "p", "v" => [o.to_s]} : o }
p tagger.dump({"outer" => Point.new(5, 6)})

# on_load runs innermost first
reviver = JSON::Coder.new(on_load: ->(v) { v.is_a?(Hash) && v["t"] ? [:tagged, v["v"]] : v })
p reviver.load('{"t":1,"v":[{"t":1,"v":2}]}')
