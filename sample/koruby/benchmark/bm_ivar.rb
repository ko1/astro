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

def bench
  c = Counter.new
  i = 0
  while i < 32_000
    c.incr
    i += 1
  end
  c.count
end

result = 0
i = 0
while i < 1000
  result = bench
  i += 1
end
p(result)
