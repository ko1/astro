# Hash#lazy iterates the [k,v] pairs (the lazy driver only handled Array/Range
# sources before, so a Hash source produced nothing). vs ruby.
p({a: 1, b: 2, c: 3}.lazy.map { |k, v| v }.to_a)
p({a: 1, b: 2, c: 3}.lazy.select { |k, v| v > 1 }.to_a)
p({a: 1, b: 2}.lazy.to_a)
p({a: 1, b: 2}.lazy.first(1))
p({a: 1, b: 2, c: 3}.lazy.map { |k, v| [k, v * 10] }.first(2))
p({x: 5}.lazy.reject { |k, v| v > 10 }.to_a)
