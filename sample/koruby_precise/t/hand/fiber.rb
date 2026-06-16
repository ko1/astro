f = Fiber.new do
  puts "a"
  Fiber.yield 1
  puts "b"
  Fiber.yield 2
  puts "c"
  3
end
p f.resume
p f.resume
p f.resume
p f.alive?
# value passing into resume
g = Fiber.new do |x|
  y = Fiber.yield(x * 2)
  z = Fiber.yield(y * 2)
  z * 2
end
p g.resume(5)
p g.resume(10)
p g.resume(20)
# generator pattern
gen = Fiber.new do
  i = 0
  while true
    Fiber.yield i
    i += 1
  end
end
p [gen.resume, gen.resume, gen.resume, gen.resume]
