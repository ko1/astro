m = "hello".method(:upcase)
p m.inspect.start_with?("#<Method:")
p m.inspect.include?("upcase")
p m.inspect.include?("String")
p [1,2,3].method(:push).inspect.start_with?("#<Method:")
p 42.method(:+).inspect.include?("Integer")
p "hi".method(:upcase).class
p :sym.to_proc.class
