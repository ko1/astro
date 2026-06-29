class MyArr < Array; end
a = MyArr[1, 2, 3]
p a.dup.class
p a.dup
p a.clone.class
class MyHash < Hash; end
h = MyHash[:a, 1]
p h.dup.class
p h.dup
class MyStr < String; end
ms = MyStr.new("hi")
p ms.dup.class
p [1, 2].dup
p({ x: 1 }.dup)
p "hi".dup
p Object.new.dup.class
