class H < Hash; end
h = H.new(99)
p h["x"]
p h.class
h[:a] = 1
p h[:a]
h2 = H.new { |hash, k| hash[k] = k.to_s }
p h2[5]
p h2.class
h3 = H.new
p h3["y"]
