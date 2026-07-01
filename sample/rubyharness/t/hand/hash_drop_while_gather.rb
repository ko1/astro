# Hash#drop_while gathers the [k,v] pair for a 1-param block (like each/map/
# take_while), unlike select/reject which spread. vs ruby.
p({a: 1, b: 2, c: 3}.drop_while { |k, v| v < 2 })
p({a: 1, b: 2, c: 3}.take_while { |k, v| v < 3 })
p({a: 1, b: 2}.drop_while { |pair| pair[1] < 2 })
p({a: 1, b: 2, c: 3}.drop_while { |k, v| k == :a })
g = []
{a: 1, b: 2}.drop_while { |x| g << x; false }
p g
p({a: 1, b: 2}.map { |x| x })
p({a: 1, b: 2}.select { |x| x })
