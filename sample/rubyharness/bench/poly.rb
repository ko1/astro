# megamorphic call site: area called on 4 different shape classes
class Sq;  def initialize(a); @a = a; end; def area; @a * @a; end; end
class Rect; def initialize(a, b); @a = a; @b = b; end; def area; @a * @b; end; end
class Tri; def initialize(a, b); @a = a; @b = b; end; def area; @a * @b / 2; end; end
class Cir; def initialize(r); @r = r; end; def area; @r * @r * 3; end; end

def bench
  shapes = [Sq.new(3), Rect.new(3, 4), Tri.new(6, 5), Cir.new(2)]
  s = 0; i = 0
  while i < 80_000
    s += shapes[i & 3].area
    i += 1
  end
  s
end

result = 0
i = 0
while i < 100
  result = bench
  i += 1
end
p result
