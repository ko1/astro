D = Data.define(:x, :y)
p D[1, 2].x
p D[x: 5, y: 6].y
S = Struct.new(:a, :b)
p S[10, 20].a
p S[7].a
p D[1,2] == D.new(1,2)
