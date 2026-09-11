# Hash#== / #eql? match keys through #hash + #eql? of user objects.
class K
  attr_reader :v
  def initialize(v) = @v = v
  def hash = @v.hash
  def eql?(o) = o.is_a?(K) && o.v == @v
end
p({ K.new(1) => :a } == { K.new(1) => :a })
p({ K.new(1) => :a }.eql?({ K.new(1) => :a }))
p({ K.new(1) => :a } == { K.new(2) => :a })
p({ K.new(1) => { K.new(2) => 3 } } == { K.new(1) => { K.new(2) => 3 } })

class Ident
  def hash = 0
  def eql?(o) = equal?(o)
end
a, b = Ident.new, Ident.new
p({ a => 1 } == { b => 1 })
p({ a => 1 } == { a => 1 })

class Boom
  def hash = raise("boom")
end
begin
  { Boom.new => 1 } == { Boom.new => 1 }
rescue => e
  puts e.message
end
h = { 1 => 2, "a" => [1], [1, 2] => :x }
p h == { 1 => 2, "a" => [1], [1, 2] => :x }, h.eql?(h.dup)
