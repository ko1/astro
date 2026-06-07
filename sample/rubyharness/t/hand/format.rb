# L0: sprintf / format / % string formatting
p format("%d", 42)
p format("%05d", 42)
p format("%+d", 42)
p format("%x", 255)
p format("%X", 255)
p format("%o", 8)
p format("%b", 5)
p format("%.2f", 3.14159)
p format("%8.3f", 3.14159)
p format("%e", 12345.678)
p format("%s", "hi")
p format("%10s", "hi")
p format("%-10s|", "hi")
p format("%c", 65)
p format("%d + %d = %d", 1, 2, 3)
p format("%%")
p sprintf("%d-%d", 1, 2)
p "%d apples" % 5
p "%s and %s" % ["a", "b"]
p "%04.1f" % 2.5
p "%#x" % 255
p "value: %3d" % 7
