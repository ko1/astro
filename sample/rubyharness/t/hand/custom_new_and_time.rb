# A user-defined `def self.new` overrides the default allocator; Time.new(parts). vs ruby.
class Factory
  def self.new(x); "custom:#{x}"; end
end
p Factory.new(5)
class Cached
  @pool = {}
  def self.new(k)
    @pool[k] ||= allocate.tap { |o| o.instance_variable_set(:@k, k) }
  end
  def key; @k; end
end
a = Cached.new(:x); b = Cached.new(:x); c = Cached.new(:y)
p a.equal?(b)
p a.equal?(c)
p a.key
# normal allocation still works
class Plain
  def initialize(a, b); @a, @b = a, b; end
  def sum; @a + @b; end
end
p Plain.new(3, 4).sum
# Time.new with components
t = Time.new(2020, 6, 15, 12, 30, 45)
p [t.year, t.month, t.day, t.hour, t.min, t.sec]
p t.strftime("%Y-%m-%d %H:%M:%S")
p Time.new(2021, 1, 1).wday
