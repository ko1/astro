m = [1,2,3].method(:map)
p m.call { |x| x * 10 }
p m.() { |x| x + 1 }
p m[] { |x| x - 1 }
p [1,2,3].method(:select).call { |x| x.odd? }
p [1,2,3].method(:each_with_index).call { |x, i| }.class
p "hello".method(:gsub).call("l") { |c| c.upcase }
p [1,2,3,4].method(:reject).call { |x| x.even? }
p({a: 1, b: 2}.method(:each).call { |k, v| })
p [1,2,3].method(:inject).call { |a, b| a + b }
p [1,2,3].method(:reduce).call(10) { |a, b| a + b }
double = 5.method(:+)
p double.call(3)
class Greeter
  def greet(name); yield "Hello, #{name}"; end
end
g = Greeter.new.method(:greet)
p g.call("World") { |msg| msg.upcase }
