# keyword binding: unknown/missing keyword errors, defaults that run Ruby (with and without optionals), **rest, via .new
def k1(a:, b: 2) = [a, b]
def k2(x, y = (GC.start; :dy), k: :dk) = [x, y, k]
def k3(k: k1(a: 9)) = k
def k4(a: 1, **r) = [a, r]
def k5(p = k1(a: 5, b: 6), q: p.sum) = [p, q]
p k1(a: 1), k1(a: 1, b: 3), k1(b: 0, a: 7)
p k2(1), k2(1, 2), k2(1, k: 3), k2(1, 2, k: 4)
p k3, k3(k: :given), k4, k4(a: 2, z: 3, "s" => 4), k5, k5([1, 2]), k5(q: 0)
[-> { k1(a: 1, c: 2) }, -> { k1(a: 1, c: 2, d: 3) }, -> { k1(b: 1) }, -> { k1(a: 1, "s" => 2) },
 -> { k2(1, z: 1) }, -> { k2(1, 2, k: 1, w: 2) }, -> { k1 }].each do |f|
  f.call
rescue ArgumentError => e
  p e.message
end
class KwPoint
  attr_reader :x, :y
  def initialize(x:, y: 0) = (@x = x; @y = y)
end
p KwPoint.new(x: 1).y, KwPoint.new(y: 2, x: 3).x
begin
  KwPoint.new(x: 1, z: 2)
rescue ArgumentError => e
  p e.message
end
h = { a: 4 }
p k1(**h), k1(**h, b: 5), k4(**h, q: 1)
3.times { |i| GC.start if i == 1; p k2(i, k: i), k5([i]) }
