def t; yield; rescue ArgumentError => e; "AE: #{e.message}"; end
l1 = ->(a, b) { a + b }
p l1.call(1, 2)
p t { l1.call(1) }
p t { l1.call(1, 2, 3) }
l2 = ->(a, b = 9) { a + b }
p l2.call(1)
p t { l2.call }
p t { l2.call(1, 2, 3) }
l3 = ->(a, *b) { [a, b] }
p l3.call(1, 2, 3)
p t { l3.call }
l4 = ->() { 42 }
p t { l4.call(1) }
pr = proc { |a, b| a }       # non-lambda: lenient
p pr.call(1)
p pr.call(1, 2, 3)
p({x: 1}.to_proc.call(:x))
p t { {x: 1}.to_proc.call }
