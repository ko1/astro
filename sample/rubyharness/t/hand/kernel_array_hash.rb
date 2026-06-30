p Array(nil)
p Array([1, 2])
p Array(1)
p Array(1..3)
p Array("a")
p Array({a: 1})
p Hash(nil)
p Hash([])
p Hash({a: 1})
begin; Hash([[1, 2]]); rescue TypeError => e; p e.message; end
begin; Hash(5); rescue TypeError => e; p e.message; end
class HasToA; def to_a; [9, 8, 7]; end; end
p Array(HasToA.new)
class HasToAry; def to_ary; [1, 2]; end; end
p Array(HasToAry.new)
class HasToH; def to_hash; {x: 1}; end; end
p Hash(HasToH.new)
p Array(:sym)
p Array(0)
