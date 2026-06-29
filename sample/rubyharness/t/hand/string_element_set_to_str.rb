s = "hello".dup
s[0] = "H"
p s
class TS; def to_str; "XY"; end; end
t = "world".dup
t[0] = TS.new
p t
u = "abcdef".dup
u[1, 2] = "ZZ"
p u
v = "hello".dup
v["ll"] = "LL"
p v
def rescue_e; yield; rescue TypeError; "TE"; rescue IndexError; "IE"; rescue FrozenError; "FE"; end
p rescue_e { "x".dup[0] = 5 }
p rescue_e { "x".dup[10] = "y" }
p rescue_e { "x".freeze[0] = "y" }
p rescue_e { "abc".dup[20] = 5 }
r = ("abc".dup[0] = TS.new)
p r.class
