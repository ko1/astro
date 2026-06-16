p Integer("30")
p Integer("0x1a")
p Integer("0b101")
p Integer("0o17")
p Integer("010")
p Integer("1_000")
p Integer(" 42 ")
p Integer("-17")
p Integer("+8")
p Integer(3.9)
p Integer(-3.9)
p Integer(42)
p Integer("ff", 16)
p Integer("z", 36)
p Float("3.14")
p Float("2")
p Float(5)
p Float("1e3")
["1", "22", "333"].each { |s| p Integer(s) }
begin; Integer("x"); rescue => e; puts e.class; end
begin; Integer("12x"); rescue => e; puts e.class; end
begin; Integer(nil); rescue => e; puts e.class; end
begin; Float("y"); rescue => e; puts e.class; end
