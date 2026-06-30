class Tree; include Enumerable; def initialize(*v); @v=v; end; def each(&b); @v.each(&b); end; end
t = Tree.new(5, 3, 8, 1)
p t.select.with_index { |x, i| i.even? }
p t.reject.with_index { |x, i| i.even? }
p t.flat_map.with_index { |x, i| [x, i] }
p t.map.with_index { |x, i| [i, x] }
p t.select { |x| x > 3 }
p t.reject { |x| x > 3 }
p t.flat_map { |x| [x, -x] }
p t.select.to_a
p t.flat_map.to_a
p t.filter.with_index { |x, i| i.odd? }
p t.find_all.with_index { |x, i| x > i }
class Words; include Enumerable; def each; %w[apple bob cherry].each { |w| yield w }; end; end
w = Words.new
p w.select.with_index { |s, i| i > 0 }
p w.reject.with_index { |s, i| s.length > 3 }
p w.flat_map.with_index { |s, i| [s, i] }
