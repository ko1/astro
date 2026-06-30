p false.object_id
p true.object_id
p nil.object_id
p 0.object_id
p 1.object_id
p 42.object_id
p (-1).object_id
p 100.object_id
p 42.object_id == 42.object_id
p :foo.object_id == :foo.object_id
p(:foo.object_id != :bar.object_id)
p 5.__id__
p "x".object_id == "y".object_id
p :sym.object_id.is_a?(Integer)
p 7.object_id.odd?
