# L0: boolean / comparison / misc operators
p true && false
p true || false
p !true
p !nil
p nil && 1
p nil || 2
p 1 && 2
p false or true
p (not false)
p 1 == 1.0
p 1.eql?(1)
p 1.eql?(1.0)
p 1.equal?(1)
p "a".equal?("a")
p :a.equal?(:a)
p nil.nil?
p 1.nil?
p nil.to_a
p nil.to_s
p nil.inspect
p 5.between?(1, 10)
p 5.clamp(1, 3)
p [1, 2, 3] <=> [1, 2, 4]
p "abc".frozen?
p 1.frozen?
p :sym.frozen?
p defined?(x)
y = 1
p defined?(y)
p defined?(puts)
p defined?(String)
p (1..3).to_a == [1, 2, 3]
p 5 & 3
p 5 | 2
p 5 ^ 1
p 10 % 3
p(-10 % 3)
p 2 <=> 3
p "a" * 0
p [] + []
p({}.merge({}))
