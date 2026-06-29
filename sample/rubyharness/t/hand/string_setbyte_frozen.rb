s = "hello".dup
s.setbyte(0, 72)
p s
def t; yield; rescue FrozenError; "FE"; end
p t { "hi".freeze.setbyte(0, 65) }
