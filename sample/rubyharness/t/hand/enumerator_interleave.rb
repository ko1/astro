# A chained Enumerable method on an Enumerator runs its block INSIDE the
# source's yield: the source's state between two yields is observable (that is
# how rubocop-ast's NodePattern counts captures across union branches).
$log = []

class Src
  def gen(items)
    return to_enum(__method__, items) unless block_given?
    items.each { |i| $log << [:src, i]; yield i }
    :src_done
  end
  def pairs
    return to_enum(__method__) unless block_given?
    yield 1, :a
    yield 2, :b
  end
end

s = Src.new
$log.clear; p s.gen([1, 2]).map { |x| $log << [:map, x]; x * 10 }; p $log
$log.clear; p s.gen([1, 2, 3]).select { |x| $log << [:sel, x]; x.odd? }; p $log
$log.clear; p s.gen([1, 2, 3]).reject { |x| $log << [:rej, x]; x.odd? }; p $log
$log.clear; p s.gen([1, 2, 3]).filter_map { |x| $log << [:fm, x]; x * 2 if x.odd? }; p $log
$log.clear; p s.gen([1, 2, 3]).take_while { |x| $log << [:tw, x]; x < 3 }; p $log

# a break out of the chained block wins
p(s.gen([1, 2, 3]).map { |x| break :broke if x == 2; x })

# multi-value yields keep their spread/gather rules
p s.pairs.map { |a, b| [a, b] }
p s.pairs.select { |a, _| a.odd? }
p s.pairs.to_a

# the same source can be re-driven
e = s.gen([1, 2])
p [e.map { |x| x }, e.map { |x| x + 1 }]
