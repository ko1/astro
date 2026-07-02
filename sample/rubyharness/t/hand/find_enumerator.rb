# Array#find / #detect without a block returns an Enumerator that drives with
# find semantics (early-stop, returns the first match / nil). vs ruby.
p [1, 2, 3, 4, 5].find.with_index { |x, i| x > 3 }
p [1, 2, 3, 4, 5].find.with_index { |x, i| i == 2 }
p [1, 2, 3].find.with_index { |x, i| x > 10 }
p [10, 20, 30].find.each { |x| x > 15 }
p [1, 2, 3, 4].find.with_index(1) { |x, i| i == 3 }
p %w[a bb ccc].find.with_index { |s, i| s.length == i + 1 }
p [1, 2, 3].detect.with_index { |x, i| x == 2 }
e = [5, 10, 15].find
p e.class
p e.each { |x| x > 7 }
p [1, 2, 3, 4, 5].find { |x| x > 3 }
p [1, 2, 3].find { |x| x > 10 }
