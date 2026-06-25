# self-referential inspect (depth-capped, compared via include?) + finite lazy (rubyspec follow-up)
a = []
a << a
p a.inspect.include?("[...]")
p a.hash.is_a?(Integer)
p (a.hash == a.hash)
p (a <=> a)

h = {}
h[:k] = h
p h.inspect.include?("{...}")
p h.hash.is_a?(Integer)

# finite lazy enumerator: each / with_index / select / map all terminal-materialize
p [1, 2, 3].lazy.map { |x| x * 10 }.to_a
out = []
[1, 2, 3].lazy.map { |x| x * 2 }.each { |v| out << v }
p out
p [1, 2, 3, 4, 5].lazy.select(&:odd?).to_a
p [10, 20, 30].each.with_index(1).to_a
