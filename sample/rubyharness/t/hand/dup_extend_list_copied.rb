# A dup/clone gets its own copy of the extended-modules list: extending the copy
# leaves the original alone.
module Base; def hi = "Base"; end
module A; extend Base; def self.own = 1; end
B = A.dup; B.extend(Module.new { def hi = "Ext" })
p A.hi, B.hi, B.own, A.singleton_class.ancestors.size == B.singleton_class.ancestors.size - 1
o = Object.new; o.extend(Base); o2 = o.clone; o2.extend(Module.new { def hi = "O2" }); p o.hi, o2.hi
C = Class.new { extend Base }; D = C.dup; D.extend(Module.new { def hi = "D" }); p C.hi, D.hi
