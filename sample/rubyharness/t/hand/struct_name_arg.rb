Struct.new("Animal", :name)
s = Struct::Animal.new("dog")
p s.name
p Struct::Animal.members
Anon = Struct.new(nil, :a, :b)
p Anon.new(1, 2).a
p Anon.new(1, 2).b
def t; yield; rescue NameError; "NE"; end
p t { Struct.new("foo", :x) }
p t { Struct.new("123Bad", :x) }
Reg = Struct.new(:x, :y)
p Reg.new(1, 2).x
p Struct.new("Bar", :z).new(5).z
