# L0: hash access, mutation, query (no blocks)
h = { "a" => 1, "b" => 2, "c" => 3 }
p h["a"]
p h["z"]
p h.fetch("b")
p h.fetch("z", -1)
p h.key?("a")
p h.key?("z")
p h.has_value?(2)
p h.length
p h.size
p h.empty?
p({}.empty?)
p h.keys
p h.values
p h.to_a
p h.invert
h["d"] = 4
p h
h.store("e", 5)
p h
p h.delete("a")
p h
p h.merge({ "x" => 10 })
g = { a: 1, b: 2 }
p g[:a]
p g.keys
p Hash.new(0)["missing"]
counts = Hash.new(0)
counts["x"] += 1
counts["x"] += 1
counts["y"] += 1
p counts
p({ a: 1 } == { a: 1 })
p({ a: 1, b: 2 }.each_pair.to_a)
