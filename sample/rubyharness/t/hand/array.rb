# L0: array indexing, mutation, query (no blocks)
a = [1, 2, 3, 4, 5]
p a[0]
p a[-1]
p a[1, 2]
p a[1..3]
p a[1...3]
p a.first
p a.last
p a.first(2)
p a.last(2)
p a.length
p a.size
p a.empty?
p [].empty?
p a.include?(3)
p a.index(3)
p a.min
p a.max
p a.sum
p a.reverse
p a.sort
p [3, 1, 2].sort
p [3, 1, 2].sort.reverse
p a + [6, 7]
p a - [2, 4]
p [1, 2] * 2
p [1, 2, 2, 3, 3].uniq
p [1, [2, [3]]].flatten
p [1, 2, 3].join("-")
p [1, 2, 3, 4].take(2)
p [1, 2, 3, 4].drop(2)
p [1, 2, 3].rotate
p a.slice(1, 2)
b = [1, 2, 3]
b << 4
p b
b.push(5, 6)
p b
p b.pop
p b
p b.shift
p b
b.unshift(0)
p b
b[0] = 99
p b
b[1, 2] = [7, 8, 9]
p b
p [1, 2, 3].zip([4, 5, 6])
p [[1, 2], [3, 4]].transpose
p Array.new(3, 0)
p Array.new(3)
p (1..5).to_a
p [1, 2, 3] == [1, 2, 3]
p [1, nil, 2, nil].compact
