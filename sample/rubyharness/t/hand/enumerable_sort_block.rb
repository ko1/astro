class Stack; include Enumerable; def initialize(*i); @i=i; end; def each; @i.each { |x| yield x }; end; end
s = Stack.new(3, 1, 2, 5, 4)
p s.sort { |a, b| b <=> a }
p s.sort
p s.sort_by { |x| -x }
p s.each_entry.to_a
p s.min(2)
p s.max(2)
p s.min { |a, b| a <=> b }
p s.sort.reverse
p({c: 3, a: 1, b: 2}.sort)
p({c: 3, a: 1, b: 2}.sort { |x, y| y[1] <=> x[1] })
class Words; include Enumerable; def each; %w[banana apple cherry].each { |w| yield w }; end; end
w = Words.new
p w.sort
p w.sort { |a, b| a.length <=> b.length }
p w.sort_by(&:length)
p w.max { |a, b| a.length <=> b.length }
p w.min_by(&:length)
