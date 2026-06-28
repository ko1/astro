class C; def initialize; @x = 5; end; end
o = C.new
p o.instance_variable_defined?(:@x)
p o.instance_variable_defined?(:@y)
p o.instance_variable_defined?("@x")
p o.remove_instance_variable(:@x)
p o.instance_variable_defined?(:@x)
p (begin; o.remove_instance_variable(:@y); rescue NameError; "NE"; end)
p (begin; o.instance_variable_defined?(:bad); rescue NameError; "NE"; end)
