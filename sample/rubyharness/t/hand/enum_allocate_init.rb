# Enumerator.allocate + #initialize sets up a generator (was a SEGV: allocate
# produced a generic object that enumerator methods VAL2ENUM-cast). vs ruby.
e = Enumerator.allocate
e.send(:initialize) do |y|
  y.yield 3
  y << 2 << 1
end
r = []
e.each { |x| r << x }
p r
p e.first(2)
p e.class == Enumerator
# initialize without a block raises ArgumentError
e2 = Enumerator.allocate
begin
  e2.send(:initialize)
rescue ArgumentError => ex
  p ex.class
end
