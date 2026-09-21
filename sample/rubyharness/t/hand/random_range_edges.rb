# Random#rand with a Range: an INCLUSIVE Float range draws from CRuby's closed
# [0,1] generator (int_pair_to_real_inclusive), not the half-open one, so the
# value was off in the 8th digit.  A zero-width inclusive range draws nothing at
# all, an empty/reversed one is an ArgumentError (Random#rand only), and an
# infinite bound is Errno::EDOM.
seeds = [1, 2, 12345, 99999]
ranges = ["1.0..2.0", "0.0..1.0", "-5.0..5.0", "1.0...2.0", "0.0...1.0",
          "-1.5...3.25", "1..2.0", "1.0..2", "0.0..0.0", "3.5..3.5", "1..10", "1...10"]
seeds.each do |s|
  ranges.each do |src|
    r = Random.new(s)
    p [s, src, (r.rand(eval(src)) rescue $!.class), r.rand]   # the trailing draw pins stream alignment
  end
end
[2.0..1.0, 2..1, 2...2, 2.0...2.0, 0.0...0.0,
 1.0..Float::INFINITY, -Float::INFINITY..0.0].each do |rg|
  p [rg.to_s,
     (Random.new(1).rand(rg) rescue [$!.class, $!.message]),
     (rand(rg) rescue [$!.class, $!.message])]
end
r = Random.new(7); p Array.new(6) { r.rand(0.0..10.0) }
r = Random.new(7); p Array.new(6) { r.rand(0.0...10.0) }
srand(3); p [rand(1.0..2.0), rand(0.5...0.75), rand]
