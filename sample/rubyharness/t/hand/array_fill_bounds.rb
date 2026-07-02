# Array#fill refuses an absurd size (ArgumentError) and a Bignum length
# (RangeError) instead of looping to exhaustion. vs ruby.
arr = [1, 2, 3]
begin; arr.fill(10, 1, 2**62 - 1); rescue ArgumentError, RangeError => e; p e.class; end
begin; arr.fill(10, 1, 2**64); rescue RangeError; p :range; rescue => e; p e.class; end
p [1, 2, 3, 4].fill("a", 3, -10000)
p [0, 0, 0].fill(9)
p [0, 0, 0].fill(9, 1)
p [1, 2, 3].fill { |i| i * i }
p [1, 2, 3, 4, 5].fill(0, 2, 2)
