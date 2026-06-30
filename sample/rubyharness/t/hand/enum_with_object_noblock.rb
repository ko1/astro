p [1,2,3,4,5].each_slice(2).with_object([]).to_a
p [1,2,3].each.with_object("x").to_a
p [1,2,3].map.with_object([]).to_a
p [1,2,3].each_slice(2).with_object([]) { |slice, memo| memo << slice }
p [1,2,3].each.with_object(0).class
p [1,2,3].each_cons(2).with_object([]).to_a
r = [1,2,3].each.with_object([]) { |x, memo| memo << x * 2 }
p r
p [10,20].each.with_object({}).to_a
