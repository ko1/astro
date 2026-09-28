require 'date'
d1 = Date.new(2020, 1, 1); d2 = Date.new(2020, 1, 2)
p [[d2, "b"], [d1, "a"]].sort.size rescue p $!
p [[d2], [d1]].sort.size rescue p $!
p [d2, d1].sort.size rescue p $!
p [[1, d2], [1, d1]].sort.size rescue p $!
class Foo; include Comparable; attr_reader :v; def initialize(v) = @v = v; def <=>(o) = v <=> o.v; end
p [[Foo.new(2)], [Foo.new(1)]].sort.map { |x| x[0].v } rescue p $!
require 'date'
d1 = Date.new(2020, 1, 1); d2 = Date.new(2020, 1, 2)
a = [[d2, "b"], [d1, "a"]]
p a.max.first.day, a.min.first.day, a.minmax.map { |x| x.first.day }, a.sort_by { |x| x }.first.first.day
b = a.dup; b.sort!; p b.first.first.day
p a.max(1).size, a.min_by { |x| x }.first.day, a.each_slice(1).sort.size, a.to_set.sort.first.first.day
require 'set'
p [[d2], [d1]].sort.reverse.first.first.day, [[d1, 1], [d1, 0]].sort.map(&:last)
a = (1..2000).map { |i| (i * 7919 % 2000).to_s }; b = a.dup; b.sort!; p b == a.sort, b.first(3); c = a.dup; c.sort! { |x, y| y <=> x }; p c.first(2); p [3,1,2].sort!.frozen?, ([1,2].freeze.sort! rescue $!.class)
p(([1, "a"].sort rescue $!.message), ([[1], ["a"]].sort rescue $!.message), [[2, "b"], [1, "a"]].sort, ([3, nil, 1].sort rescue $!.message), ([Object.new, Object.new].sort rescue $!.class))
