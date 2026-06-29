l = ->(x) { x }
p l.inspect.start_with?("#<Proc:0x")
p l.inspect.include?("(lambda)")
pr = proc { |x| x }
p pr.inspect.start_with?("#<Proc:0x")
p pr.inspect.include?("(lambda)")
p ->(x){x*2}.curry.inspect.start_with?("#<Proc")
m = [1,2,3].each
p l.is_a?(Proc)
p({a: 1}.inspect)
p [1, "two", :three].inspect
