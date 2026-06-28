S = Struct.new(:a, :b, :c)
s = S.new(1, 2, 3)
case s; in {a:, b:}; p [a, b]; end
case s; in {a: 1, c:}; p c; end
case s; in {z:}; p "z"; else; p "no-z"; end
D = Data.define(:x, :y)
case D.new(10, 20); in {x:, y:}; p [x, y]; end
case({a: 1, b: 2}); in {a:, b:}; p [a, b]; end
