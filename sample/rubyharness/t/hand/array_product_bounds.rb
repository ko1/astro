# Array#product raises RangeError for an unreasonable product count instead of
# overflowing/hanging. vs ruby.
a = (0..100).to_a
begin; a.product(a, a, a, a, a, a, a, a, a, a); rescue RangeError; p :range; end
p [1, 2].product([3, 4])
p [1, 2].product([3, 4], [5, 6]).length
p [1, 2].product([]).length
cnt = 0; [1, 2, 3].product([4, 5]) { |x| cnt += 1 }; p cnt
p [1].product
