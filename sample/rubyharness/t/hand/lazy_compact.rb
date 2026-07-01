# Enumerator::Lazy#compact drops nils lazily (no block). vs ruby.
p [1, nil, 2, nil, 3].lazy.compact.first(2)
p [nil, nil, 1, 2, nil, 3].lazy.compact.to_a
p [1, 2, 3].lazy.map { |x| x.even? ? x : nil }.compact.to_a
p (1..10).lazy.map { |x| x if x.odd? }.compact.first(3)
p [1, nil, 2].lazy.compact.map { |x| x * 10 }.to_a
p [nil, nil].lazy.compact.to_a
