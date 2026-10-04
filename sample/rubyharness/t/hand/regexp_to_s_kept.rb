# Regexp#to_s keeps its text between calls but every call returns a new, mutable String
re = /a(b|c)+\d{2,}/ix
s1 = re.to_s
s2 = re.to_s
p s1, s1 == s2, s1.equal?(s2), s1.frozen?
s1 << "!"
p re.to_s, s1
p (/x#{re}y/).source
p Regexp.new("é+", Regexp::IGNORECASE).to_s.encoding, /é/.to_s, /é/.to_s.encoding
d = re.dup
p d.to_s == re.to_s, d.to_s.equal?(re.to_s)
class MyRe < Regexp; end
m = MyRe.new("q+")
p m.to_s, m.to_s
3.times { GC.start; p re.to_s }
p Regexp.union(/a/, /b/i).to_s
