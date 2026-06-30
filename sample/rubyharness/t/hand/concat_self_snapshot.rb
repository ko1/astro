# Array#concat / String#concat with self as an argument append the original
# (snapshot), not the growing, self. vs ruby.
a = [1, 2]; a.concat(a, a); p a
b = [1, 2]; b.concat(b); p b
c = [1, 2]; c.concat([3], c, [4]); p c
p [1, 2].concat([3, 4])
s = "hello"; s.concat(s, s); p s
t = "ab"; t.concat(t); p t
u = "x"; u.concat("y", u, "z"); p u
p "hi".concat(" ", "there")
n = "z"; n.concat(33); p n
