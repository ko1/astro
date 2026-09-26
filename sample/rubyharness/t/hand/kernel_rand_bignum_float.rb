# Kernel#rand with a Bignum draws an Integer below it (its magnitude when negative).
p [rand(2**64).class, rand(-(2**64)).class, (0...20).all? { rand(2**70) < 2**70 }, Random.new(1).rand(2**64).class, rand(2**64 + 0.5).class]
p [rand(1.5).class, rand(3.9) < 3, rand(0.5).class, rand(-2.5).class, Random.new(1).rand(1.5).class, rand(1e20).class]
