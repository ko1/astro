# A dup of a builtin class makes real instances of it (String.dup.new("x") is a String-like object).
O = String.dup
s = O.new("new")
p s.class == O, s.is_a?(String), s.respond_to?(:to_str), s.to_str, s + "!", s.size, O.allocate.class == O
A = Array.dup; a = A.new([1, 2]); p a.class == A, a.size, a.map { _1 * 2 }
H = Hash.dup; h = H.new; h[:k] = 1; p h.class == H, h[:k]
K = Class.new { def hi = :hi }.dup; p K.new.hi
