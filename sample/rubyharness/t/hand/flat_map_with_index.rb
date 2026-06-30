p [1,2,3].flat_map.with_index { |x, i| [x, i] }
p [1,2,3].flat_map { |x| [x, -x] }
p [1,2,3].flat_map.with_index { |x, i| [x] * i }
p [1,2,3].flat_map.each { |x| [x, x*10] }
p [1,2,3].flat_map.to_a
p [[1,2],[3,4]].flat_map.with_index { |a, i| a + [i] }
p [1,2,3].flat_map.with_index { |x, i| x }
p [1,2,3].map.with_index { |x, i| x * i }
p [10,20,30].select.with_index { |x, i| i.even? }
p [1,2,3,4].reject.with_index { |x, i| i.even? }
p [1,2,3].collect_concat.with_index { |x, i| [x, i] }
p %w[a b c].flat_map.with_index { |s, i| [s, i.to_s] }
p [1,2,3].flat_map.with_index(10) { |x, i| [x, i] }
