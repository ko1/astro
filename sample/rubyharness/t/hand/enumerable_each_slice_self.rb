# Enumerable#each_slice / #each_cons return self (the receiver) when a block is
# given, not nil. vs ruby.
class N
  include Enumerable
  def each; [1, 2, 3, 4, 5].each { |x| yield x }; end
end
n = N.new
p(n.each_slice(2) { |s| }.equal?(n))
p(n.each_cons(2) { |c| }.equal?(n))
collected = []
r = n.each_slice(2) { |s| collected << s }
p collected
p r.equal?(n)
p [1, 2, 3, 4].each_slice(2) { |s| }
p [1, 2, 3, 4].each_cons(2) { |c| }
p n.each_slice(2).to_a
p n.each_cons(2).to_a
