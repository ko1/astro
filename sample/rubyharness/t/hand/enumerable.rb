# L1: broader Enumerable / iteration coverage
p [1, 2, 3, 4, 5].each_cons(2).to_a
p [1, 2, 3, 4, 5].each_slice(2).to_a
p [1, 2, 3].cycle.first(7)
p [1, 2, 3, 4].chunk_while { |a, b| b - a == 1 }.to_a
p [1, 1, 2, 3, 3, 3].chunk { |x| x }.to_a
p [1, 2, 3, 1, 2].tally
p [1, 2, 3].zip([4, 5, 6], [7, 8, 9])
p [1, 2, 3].flat_map { |x| [x, -x] }
p %w[apple banana cherry].group_by { |s| s.length }
p [1, 2, 3, 4].partition(&:even?)
p [5, 3, 1, 4, 2].sort
p [5, 3, 1, 4, 2].sort { |a, b| b <=> a }
p [5, 3, 1, 4, 2].min(2)
p [5, 3, 1, 4, 2].max(2)
p [1, 2, 3, 4].minmax
p ["bb", "a", "ccc"].sort_by(&:length)
p ["bb", "a", "ccc"].max_by(&:length)
p [1, 2, 3, 4, 5].sum
p [1, 2, 3, 4].inject(:*)
p [1, 2, 3, 4].inject(10) { |a, x| a + x }
p [1, 2, 3].reduce(1, :*)
p [1, 2, 3, 4].find_index(3)
p [1, 2, 3, 4].count(&:odd?)
p [[1, 2], [3, 4]].to_h
p (1..10).select(&:even?)
p (1..5).map { |i| i * i }
p (1..Float::INFINITY).lazy.select(&:even?).first(3)
p [1, 2, 3].each_with_index.to_a
p [10, 20, 30].each_with_object({}) { |x, h| h[x] = x * 2 }
p [1, 2, 3, 4].filter_map { |x| x * 2 if x.even? }
p ["a", "b", "c"].each_with_index.map { |c, i| "#{i}:#{c}" }
p [3, 1, 2].sort!.frozen?
arr = [3, 1, 2]
arr.map! { |x| x * 10 }
p arr
arr.select! { |x| x > 10 }
p arr
p [1, 2, 3, 4].take(2)
p [1, 2, 3, 4].drop(2)
p [1, 2, 3].first
p [1, 2, 3].include?(2)
p (1..100).step(10).to_a
p ["x", "y"].each.with_index(1).to_a
