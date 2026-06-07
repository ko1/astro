# L0: type conversions and predicates
p 42.to_s
p 3.14.to_s
p "42".to_i
p "3.14".to_f
p "42abc".to_i
p "abc".to_i
p "  10  ".to_i
p 65.chr
p "A".ord
p Integer("42")
p Integer("ff", 16)
p Integer("101", 2)
p Float("3.14")
p Array(nil)
p Array([1, 2])
p Array(1)
p String(42)
p 42.inspect
p "x".inspect
p nil.inspect
p [1, "a", :b].inspect
p :sym.to_s
p "sym".to_sym
p 1.to_r
p 1.5.to_r
p 3.to_c
p [1, 2].to_a
p (1..3).to_a
p({ a: 1 }.to_a)
p [[1, 2], [3, 4]].to_h
p 42.is_a?(Integer)
p 42.is_a?(Numeric)
p 3.14.is_a?(Float)
p "x".is_a?(String)
p [].is_a?(Array)
p({}.is_a?(Hash))
p :s.is_a?(Symbol)
p nil.is_a?(NilClass)
p true.is_a?(TrueClass)
p 1.kind_of?(Integer)
p 1.instance_of?(Integer)
p 42.class
p 3.14.class
p "x".class
p :s.class
p [].class
p({}.class)
p nil.class
p true.class
p (1..2).class
