# Struct#to_h/Data#to_h with a block coerce the yielded pair via #to_ary. vs ruby.
S = Struct.new(:a, :b)
s = S.new(1, 2)
class P; def initialize(k, v); @k = k; @v = v; end; def to_ary; [@k, @v]; end; end
p s.to_h { |k, v| P.new(k, v * 10) }
p s.to_h { |k, v| [k, v * 2] }
D = Data.define(:x, :y)
d = D.new(3, 4)
p d.to_h { |k, v| P.new(k, v + 1) }
begin; s.to_h { |k, v| Object.new }; rescue => e; p e.class; end
begin; s.to_h { |k, v| [1, 2, 3] }; rescue => e; p e.class; end
