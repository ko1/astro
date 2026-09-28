p 2 ** -3, 2 ** -62, 2 ** -63, 2 ** -64, (-2) ** -65, 10 ** -20, 1 ** -(2**64), (-1) ** -(2**64+1), (2**70) ** -1, (-(2**70)) ** -2, (-(2**70)) ** -1
p 2.pow(-70), Rational(2) ** -70, (2 ** -(10**12) rescue $!.class)
p (1..3).flat_map, (1..3).flat_map.size, (1..3).find_index, (1...1).find_index, (1..3).find_index.each { |x| x == 2 }, (1..3).flat_map.with_index.to_a, ("a".."c").flat_map.to_a
