module M; X = 1; Y = 2; end
p M.const_defined?(:X)
p (M.send(:remove_const, :X))
p M.const_defined?(:X)
p (begin; M.send(:remove_const, :Z); rescue => e; e.class; end)
class C; A = 10; end
C.send(:remove_const, :A)
p C.const_defined?(:A)
