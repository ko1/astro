# Two Marshal gaps:
#  * `marshal_load` was only called when PUBLIC.  CRuby uses rb_funcall, which
#    ignores visibility — OpenStruct#marshal_load is private, so a round-trip
#    silently produced an object with no @table at all.
#  * Set is a builtin here with no @hash ivar, so it dumped as an EMPTY generic
#    object.  CRuby dumps `o:Set` carrying @hash; match that on both sides.
require 'ostruct'
require 'set'

o = OpenStruct.new(name: "John", age: 70, nested: OpenStruct.new(x: 1))
o2 = Marshal.load(Marshal.dump(o))
p [o2.class, o2.instance_variables, o2.name, o2.age, o2.nested.x, o2 == o]

class PrivMD
  def initialize(v) = @v = v
  attr_reader :v
  private def marshal_dump = @v
  private def marshal_load(v) = @v = v
end
p Marshal.load(Marshal.dump(PrivMD.new([1, {a: 2}]))).v

class PubMD
  def initialize(v) = @v = v
  attr_reader :v
  def marshal_dump = @v
  def marshal_load(v) = @v = v
end
p Marshal.load(Marshal.dump(PubMD.new("z"))).v

[Set[], Set[1, 2, 3], Set["a", :b, nil], Set[Set[1], Set[2]]].each do |s|
  r = Marshal.load(Marshal.dump(s))
  p [r.class, r.size, r == s, r.to_a.map(&:class)]
end
class MySet < Set; end
m = Marshal.load(Marshal.dump(MySet[1, 2]))
p [m.class, m.to_a, m == MySet[1, 2]]
# a Set nested in other containers
p Marshal.load(Marshal.dump({ s: Set[1, 2], a: [Set[3]] }))
# shared references survive
s1 = Set[1]
pair = Marshal.load(Marshal.dump([s1, s1]))
p [pair[0] == pair[1], pair[0].equal?(pair[1])]
