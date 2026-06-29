class MyArray < Array; end
a = MyArray[1, 2, 3]
p a.class
p a
p a[0]
p a.rotate.class
b = MyArray[]
p b.class
p b
p [10, 20][1]
