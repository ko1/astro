# An enumerator from an each-like method (each / each_with_index / times /
# each_key ...) returns the receiver from #each {} and #with_index {}, like the
# method itself would; a map enumerator returns the block results.
p [2].each.each { :x }, [2].each.with_index(1) { :x }, [2].each_with_index.with_index { :x }
p 3.times.each { :x }, 3.times.with_index { :x }, [1, 2].reverse_each.each { :x }, [4].each_index.each { :x }
p (1..2).each.each { :x }, (1..2).each_with_index.with_index { :x }, (1..2).map.each { _1 * 2 }
p({a: 1}.each.each { :x }, {a: 1}.each_key.with_index { :x }, {a: 1}.each_value.each { :x })
p [2].map.each { :x }, [2].map.with_index { |x, i| [x, i] }, [2, 3].select.with_index { |x, i| i > 0 }
e = [5, 6].each
p e.dup.each { }, e.next, e.size, e.with_index.to_a
p [[1, 2]].each.with_index { |(a, b), i| }, [[1, 2]].each.each { |a, b| [a, b] }
