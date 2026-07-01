# Object#<=> returns 0 when self == other (dispatching a custom #==), else nil.
# vs ruby.
o = Object.new
p(o <=> o)
p(o <=> Object.new)
class E; def ==(x); true; end; end
p(E.new <=> E.new)
p(E.new <=> 42)
class NE; def ==(x); false; end; end
p(NE.new <=> NE.new)
p(1 <=> 1)
p("x" <=> "x")
p(:s <=> :s)
p(nil <=> nil)
