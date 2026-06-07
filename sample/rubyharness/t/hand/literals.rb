# L0: literals — output golden is CRuby. No def/block/class.
p 0
p 1
p(-1)
p 42
p 1_000_000
p 0xff
p 0b1010
p 0o17
p 3.14
p(-2.5)
p 1.0
p 1e3
p 1.5e-2
p true
p false
p nil
p "hello"
p ""
p "tab\there"
p "new\nline"
p 'single'
p :sym
p :"with space"
p [1, 2, 3]
p []
p [1, "two", :three, [4]]
p({})
p({ "a" => 1, "b" => 2 })
p({ a: 1, b: 2 })
p (1..5)
p (1...5)
p 'a'..'e'
puts "plain"
puts 123
print "no newline"
print "\n"
