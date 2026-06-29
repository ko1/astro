class MyHash < Hash; end
h = MyHash[:a, 1, :b, 2]
p h.class
p h
p h[:a]
p h.reject { |k, v| v > 1 }
p MyHash[[[1, 2], [3, 4]]]
p MyHash[{ x: 9 }]
p MyHash[]
p Hash[:k, 5]
