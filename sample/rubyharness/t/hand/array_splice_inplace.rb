a = [1, 2, 3, 4, 5]
b = a.dup; b[1, 2] = [:x]; p b
b = a.dup; b[1, 0] = [:x, :y]; p b
b = a.dup; b[1, 1] = :z; p b
b = a.dup; b[8, 2] = [:p]; p b
b = a.dup; b[5, 0] = [:q, :r]; p b
b = a.dup; b[-2, 5] = []; p b
b = a.dup; b[0..] = [9]; p b
b = a.dup; b[2..3] = [7, 7, 7, 7]; p b
b = a.dup; b[1, 3] = b; p b
b = a.dup; b[10] = 1; p b
b = a.dup; b[1, 2] = [[1]]; p b
b = []; 1000.times { |i| b[i / 2, 0] = [i] }; p b.size, b.first(5), b.sum
b = (1..10).to_a; 5.times { b[2, 3] = [] }; p b
p((begin; x = a.dup; x[-10, 1] = 1; rescue => e; e.class; end))
