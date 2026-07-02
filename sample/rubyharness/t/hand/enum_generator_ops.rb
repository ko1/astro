# Enumerator methods on a to_enum-style generator (mode 3) and on lazy chains
# (mode 1): each_with_object, next/peek/next_values, and lazy select/reject
# without a block. vs ruby.
class Src
  include Enumerable
  def each
    yield 1; yield 2; yield 3; yield 4
  end
end
s = Src.new

# each_with_object on a generator enumerator
p s.to_enum(:each).each_with_object([]) { |x, acc| acc << x * 2 }

# external iteration: next / peek
e = s.to_enum(:each)
p e.next
p e.peek
p e.next
p e.next_values
p e.peek_values

# lazy chain + next
lz = s.to_enum(:each).lazy.select { |x| x.even? }
p lz.next
p lz.next
p lz.first(2)
p s.to_enum(:each).lazy.map { |x| x * 10 }.first(3)

# lazy select without a block raises ArgumentError
begin
  s.to_enum(:each).lazy.select
rescue ArgumentError => ex
  p ex.class
end
