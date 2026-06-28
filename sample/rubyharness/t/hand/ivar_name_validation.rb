o = Object.new
o.instance_variable_set(:@x, 5)
p o.instance_variable_get(:@x)
p (begin; o.instance_variable_set(:x, 1); rescue NameError; "NE"; end)
p (begin; o.instance_variable_set("@".to_sym, 1); rescue NameError; "NE"; end)
p (begin; o.instance_variable_set("@1".to_sym, 1); rescue NameError; "NE"; end)
p (begin; o.instance_variable_get(:foo); rescue NameError; "NE"; end)
p (begin; o.instance_variable_get("@@cv".to_sym); rescue NameError; "NE"; end)
p o.instance_variable_get(:@missing)
