h = { 1.0 => :value, "a" => :b }
p h.assoc(1)
p h.assoc(1.0)
p h.assoc("a")
p h.assoc(:missing)
class O; def ==(o); o == 99; end; def hash; 1; end; def eql?(x); false; end; end
g = { O.new => :found, 2 => :two }
pair = g.assoc(99)
p pair[1]
p pair[0].is_a?(O)
p({ a: 1, b: 2 }.assoc(:b))
