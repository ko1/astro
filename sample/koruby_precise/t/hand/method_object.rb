class Calc
  def add(a, b) = a + b
  def self.zero = 0
  def neg(x) = -x
end
c = Calc.new
m = c.method(:add)
p m.call(3, 4)
p m[10, 20]
n = c.method(:neg)
p n.call(5)
# polymorphic with Array#[] in a table
ram = [0, 0, 0]
handlers = { read: ram, write: ram.method(:[]=) }
handlers[:write][1, 99]
p handlers[:read][1]
# method on builtin receiver
sm = "hello".method(:upcase)
p sm.call
