p Array.new([1, 2, 3])
p Array.new(3, 0)
p Array.new(3) { |i| i * 2 }
p Array.new
class TA; def to_ary; [9, 8]; end; end
p Array.new(TA.new)
def t; yield; rescue => e; e.class; end
p t { Array.new(1, 2, 3) }
a = [1, 2]; b = Array.new(a); a << 3; p b
