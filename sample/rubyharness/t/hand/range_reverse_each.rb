p (1..5).reverse_each.to_a
p (1..5).reverse_each.map { |x| x * 10 }
r = []; (1..5).reverse_each { |x| r << x }; p r
p ('a'..'e').reverse_each.to_a
p (1..10).reverse_each.first(3)
p (1..5).reverse_each.select(&:even?)
p (1..5).reverse_each.class
p (1...5).reverse_each.to_a
p (1..5).reverse_each.with_index.to_a
p (1..3).reverse_each.to_a.sum
