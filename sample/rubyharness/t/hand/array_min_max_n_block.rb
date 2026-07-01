# Array#min(n)/max(n) with a comparator block sort by the block, not the default
# <=> (previously the block was ignored). vs ruby.
p [3, 1, 4, 1, 5].min(2) { |a, b| b <=> a }
p [3, 1, 4, 1, 5].max(2) { |a, b| b <=> a }
p [3, 1, 4, 1, 5].min(2)
p [3, 1, 4, 1, 5].max(2)
p [3, 1, 4, 1, 5].min { |a, b| b <=> a }
p [3, 1, 4, 1, 5].max { |a, b| b <=> a }
p ["bb", "a", "ccc"].min(2) { |a, b| a.length <=> b.length }
p ["bb", "a", "ccc"].max(2) { |a, b| a.length <=> b.length }
p [5, 3, 8, 1, 9, 2].min(3) { |a, b| a <=> b }
p [5, 3, 8, 1, 9, 2].max(3) { |a, b| a <=> b }
p({a: 1, b: 3, c: 2}.min(2))
p({a: 1, b: 3, c: 2}.max(2))
