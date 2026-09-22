# Enumerable#lazy must STREAM through #each — materialising with #to_a hangs on
# an infinite custom source, which is the whole point of lazy.
class Inf
  include Enumerable
  def each
    i = 0
    loop { yield i; i += 1 }
  end
end

p Inf.new.lazy.map { |x| x * 2 }.first(3)
p Inf.new.lazy.select(&:even?).first(2)
p Inf.new.lazy.take(3).to_a
p Inf.new.lazy.map { |x| x }.select(&:even?).first(2)
p Inf.new.lazy.class
p Inf.new.lazy.reject(&:even?).first(2)
p Inf.new.lazy.drop(2).first(2)
p Inf.new.lazy.take_while { |x| x < 4 }.to_a
p Inf.new.lazy.flat_map { |x| [x, -x] }.first(4)

# flat_map fans an Array result out one level on EVERY lazy source: the
# generator path applies ops one value at a time and used to drop the fanout.
e = Enumerator.new { |y| [3, 1, 2, 4, 5].each { |x| y << x } }
p [e.lazy.flat_map { |x| [x, -x] }.first(4),
   e.lazy.map { |x| x }.flat_map { |x| [x, -x] }.first(4),
   [3, 1, 2].lazy.flat_map { |x| [x, -x] }.first(4),
   (1..Float::INFINITY).lazy.flat_map { |x| [x, -x] }.first(4),
   [[1, [2]], [3]].lazy.flat_map { |x| x }.to_a,
   [1, 2].lazy.flat_map { |x| x }.to_a,
   [1, 2].lazy.collect_concat { |x| [x, x] }.first(3)]

# a finite one still behaves, and the core types keep their own #lazy
class Fin
  include Enumerable
  def each = [3, 1, 2].each { |x| yield x }
end
p [Fin.new.lazy.map { |x| x }.to_a, Fin.new.lazy.force, Fin.new.lazy.sort]
p [[1, 2, 3].lazy.map { |x| x * 2 }.to_a, (1..3).lazy.select(&:odd?).to_a,
   (1..Float::INFINITY).lazy.map { |x| x }.first(2)]

# A #send defined on the SINGLETON wins over the reflective one, like one on the
# class does (an rspec double stubs `send` exactly this way).
o = Object.new
def o.send(*a, &b) = [:sing, a]
p [o.send("hi"), o.send(:itself)]

class Holder2
  def initialize = (def self.send(*a) = [:sing2, a])
  def go = send("x")
end
p [Holder2.new.send("y"), Holder2.new.go]

m = Module.new { def send(*a) = [:mod, a] }
p Object.new.extend(m).send("z")

# …and an object WITHOUT one still gets the reflective send
p [Object.new.send(:itself).class, [1, 2].send(:size), 3.send(:+, 4)]
