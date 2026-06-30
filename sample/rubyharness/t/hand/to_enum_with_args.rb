p [1,2,3].each_with_object([]).to_a
p (1..3).each_with_object([]).to_a
p({a: 1, b: 2}.each_with_object([]).to_a)
class Tree; include Enumerable; def each; yield 1; yield 2; yield 3; end; end
p Tree.new.each_with_object([]).to_a
p [1,2,3].each_with_object([]).class
p [1,2,3].each_with_object([]) { |x, m| m << x * 2 }
p (1..3).each_with_object("") { |x, s| s << x.to_s }
p({a: 1}.each_with_object({}) { |(k, v), h| h[k] = v * 10 })
p [1,2,3,4,5].to_enum(:each_slice, 2).to_a
p [1,2,3,4].to_enum(:each_cons, 2).to_a
p [1,2,3].to_enum(:each_with_object, []).to_a
p "hello".enum_for(:each_char).to_a
p [1,2,3].enum_for(:each_with_index).to_a
