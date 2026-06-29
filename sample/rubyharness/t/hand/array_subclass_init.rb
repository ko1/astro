class C < Array; end
p C.new([1, 2, 3])
p C.new([1, 2, 3]).class
p C.new([1, 2, 3])[0]
p C.new(3)
p C.new(3, :x)
p C.new(3) { |i| i * 2 }
p C.new
p C.new([1, 2]).map { |x| x + 1 }
class D < Array; def initialize; super([9, 9]); end; end
p D.new
