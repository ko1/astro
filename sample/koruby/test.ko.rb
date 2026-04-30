# koruby smoke test
puts "hello, koruby"
p 1 + 2
p 3 * 4

def add(a, b)
  a + b
end
p add(10, 32)

a = [1, 2, 3, 4, 5]
p a
p a.size
p a.map { |x| x * 2 }

s = 0
[1,2,3,4,5].each { |i| s = s + i }
p s

class Foo
  def bar(x)
    x * 100
  end
end
p Foo.new.bar(5)
