a = [1, 2, 3]
def t; yield; rescue => e; e.class; end
p t { a[-5] = 9 }
p t { a[-4] = 9 }
b = [1, 2, 3, 4]
b[-1] = 0
p b
