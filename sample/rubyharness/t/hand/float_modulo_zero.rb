def t; yield; rescue ZeroDivisionError; "ZD"; end
p t { 5.0 % 0 }
p t { 5.0 % 0.0 }
p 5.0 % 2
p 5.0 % -3
p(-5.0 % 3)
p 5.5 % 2
p 5.0.modulo(2)
p(Float.instance_method(:modulo) == Float.instance_method(:%))
