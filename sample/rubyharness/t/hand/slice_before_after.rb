p [1, 2, 4, 9, 10, 11, 12, 0].slice_before { |x| x.even? }.to_a
p [0, 1, 2, 3, 4, 5].slice_after { |x| x.even? }.to_a
p [1, 2, 3, 4, 5].slice_before { |x| x % 2 == 1 }.to_a
p (1..10).slice_before { |x| x % 3 == 0 }.to_a
p [1, 2, 3].slice_before { |x| false }.to_a
p [1, 2, 3].slice_after { |x| true }.to_a
p [].slice_before { |x| true }.to_a
class Tree; include Enumerable; def each; [3, 1, 4, 1, 5].each { |x| yield x }; end; end
p Tree.new.slice_before { |x| x > 3 }.to_a
p Tree.new.slice_after(&:odd?).to_a
p [1, 2, 3, 4, 5, 6].slice_before { |x| x % 3 == 1 }.map(&:sum)
p (1..15).slice_after { |x| x % 4 == 0 }.to_a
p [5, 5, 5].slice_before { |x| x == 5 }.to_a
