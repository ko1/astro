s = "hello".dup
s.bytesplice(0, 1, "H")
p s
t = "hello".dup
t.bytesplice(1..2, "XY")
p t
u = "hello".dup
u.bytesplice(0, 2, "HELLO", 1, 3)
p u
def re; yield; rescue IndexError; "IE"; rescue FrozenError; "FE"; rescue TypeError; "TE"; rescue ArgumentError; "AE"; end
p re { "hello".dup.bytesplice(10, 1, "x") }
p re { "hello".dup.bytesplice(0, -1, "x") }
p re { "hello".freeze.bytesplice(0, 1, "x") }
p re { "hello".dup.bytesplice(0, 1, "abc", 10, 1) }
p re { "hello".dup.bytesplice(0, 1, "abc", 0, -1) }
p re { "hello".dup.bytesplice(1..3, "WORLD", 0, 2) }
