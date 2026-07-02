# Enumerable#slice_before/#slice_after: return Enumerator, pattern (===) or block
# form, and argument validation. vs ruby.
p [1,2,3,4,5].slice_before(3).class.to_s
p [1,2,3,4,5].slice_before(3).to_a
p [1,2,3,4,5].slice_before { |x| x == 3 }.to_a
p [0,1,2,3,4].slice_before(1..2).to_a
p [0,1,2,3,4].slice_after(2).to_a
p [1,2,3,4].slice_after { |x| x.even? }.to_a
p [1,2,3,4,5].slice_before(3).map { |g| g.sum }
begin; [1,2].slice_before(1) {}; rescue ArgumentError; p :both; end
begin; [1,2].slice_before; rescue ArgumentError; p :noarg; end
begin; [1,2].slice_before(1,2); rescue ArgumentError; p :toomany; end
