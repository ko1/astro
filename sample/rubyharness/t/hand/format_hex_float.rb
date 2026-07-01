# format %a/%A render a Float in hexadecimal (C printf semantics). vs ruby.
p format("%a", 1.0)
p format("%a", 0.5)
p format("%a", 255.0)
p format("%A", 1.5)
p format("%a", 0.0)
p format("%a", -2.0)
p "%a" % 1.0
p format("%a", 3.14159)
