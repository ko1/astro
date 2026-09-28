e = [1].enum_for(:map); p e; p [e]; puts "%p" % [e]
h = {a: 1, b: 2}; e = h.map; p e, e.size, e.with_index.to_a, h.flat_map.size, h.flat_map { |k, v| [k] * v }
p h.reverse_each.to_a, h.cycle.size, h.cycle(2).size, h.cycle(2).to_a, {}.cycle.size, h.map.with_index { |(k, v), i| [k, i] }
p [1, 2].each_slice(1), [1].each
