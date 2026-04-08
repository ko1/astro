# Instance variable read/write
class Counter
  def initialize
    @count = 0
  end
  def incr
    @count += 1
  end
  def count = @count
end

c = Counter.new
i = 0
while i < 36_000_000
  c.incr
  i += 1
end
p(c.count)
