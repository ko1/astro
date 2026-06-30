# sort_by / max_by / min_by dispatch <=> on user (Comparable) block keys. vs ruby.
class Rev
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); o.n <=> n; end   # reversed ordering
end
a = (1..6).map { |i| Rev.new(i) }
p a.sort.map(&:n)
p a.sort_by { |x| x }.map(&:n)
p a.max_by { |x| x }.n
p a.min_by { |x| x }.n
p [5, 1, 3, 2, 4].sort_by { |x| Rev.new(x) }
p [5, 1, 3, 2, 4].max_by { |x| Rev.new(x) }
p [5, 1, 3, 2, 4].min_by { |x| Rev.new(x) }
# scalar keys still work (fast path)
p [3, 1, 2].sort_by { |x| x }
p %w[bb a ccc].sort_by { |s| s.length }
p [{ n: 3 }, { n: 1 }, { n: 2 }].sort_by { |h| h[:n] }
p (1..10).to_a.max_by { |x| -x }
