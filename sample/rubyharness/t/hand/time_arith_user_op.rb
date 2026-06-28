a = Time.at(100); b = a + 0.001
p (a <=> b)
p (b <=> a)
p ((b - a) * 1000).round(2)
c = Time.at(100, 500)
p (a <=> c)
p c.usec
p (a <=> Time.at(100))
# user object operators with numeric rhs dispatch the object's method
class V; def +(o); "V+#{o}"; end; def -(o); "V-#{o}"; end; end
p (V.new + 1.0)
p (V.new - 2)
