a, b, = [1, 2, 3, 4]
p [a, b]
class C
  def initialize; @x, @y, = [10, 20, 30]; end
  def vals; [@x, @y]; end
end
p C.new.vals
x, = [7, 8]
p x
