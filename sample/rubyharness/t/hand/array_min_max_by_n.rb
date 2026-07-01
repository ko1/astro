# Array#min_by(n)/max_by(n) return the n smallest/largest by the block key. vs ruby.
p [1, 5, 3, 2, 4].min_by(2) { |x| x }
p [1, 5, 3, 2, 4].max_by(2) { |x| x }
p [1, 5, 3, 2, 4].min_by(3) { |x| -x }
p [1, 2, 3].min_by(10) { |x| x }
p [1, 2, 3].max_by(0) { |x| x }
p ["aaa", "b", "cc"].min_by(2, &:length)
p ["aaa", "b", "cc"].max_by(2, &:length)
p [].min_by(2) { |x| x }
# no-count form unchanged
p [1, 5, 3, 2, 4].min_by { |x| x }
p [1, 5, 3, 2, 4].max_by { |x| x }
p [1, 5, 3].min_by.class
