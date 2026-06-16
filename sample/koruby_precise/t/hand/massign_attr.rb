class Box
  attr_accessor :a, :b, :c
end
box = Box.new
x = 0
x, box.a, box.b = 1, 2, 3
p [x, box.a, box.b]
box.a, box.c = [10, 20]
p [box.a, box.c]
@iv = nil
@iv, box.a = 99, 88
p [@iv, box.a]
