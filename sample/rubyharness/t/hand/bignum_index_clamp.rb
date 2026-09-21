# koruby's Fixnum is 62-bit, so 2**62 .. 2**63-1 are Bignums that still fit a C
# long.  CRuby takes them as indices, and Integer#clamp against such a bound has
# to compare EXACTLY -- as a double, 2**63-1 rounds up to 2**63 and clamp
# returns the value unchanged.  liquid's `slice` filter clamps a length to
# exactly 2**63-1 and retries the slice, so an unchanged value loops forever.
R = (-(1 << 63))..((1 << 63) - 1)
p [R.cover?(1 << 63), R.cover?(-(1 << 64)), R.cover?(0)]
p [(1 << 63).clamp(R), (-(1 << 64)).clamp(R), (5).clamp(R)]
p [(1 << 63).clamp(R) == (1 << 63) - 1, (-(1 << 64)).clamp(R) == -(1 << 63)]
p [(1 << 62).clamp(0, (1 << 62) - 1), ((1 << 62) - 1).clamp(0, 1 << 62)]
p [3.clamp(1, 2), 0.clamp(1, 2), 1.clamp(nil, 2), 5.clamp(3, nil)]
begin; 5.clamp(3, 1); rescue ArgumentError => e; p e.message; end
begin; 5.clamp(1...3); rescue ArgumentError => e; p e.message; end

a = %w(f o o b a r)
[(1 << 62) - 1, 1 << 62, (1 << 63) - 1, 1 << 63].each do |n|
  begin
    puts "#{n}: #{a.slice(0, n).inspect}"
  rescue => e
    puts "#{n}: #{e.class}"
  end
end
p a.slice((1 << 62), 6)
p a.slice(-(1 << 62), 6)
p "foobar".slice(0, (1 << 63) - 1)
p a.first(1 << 62).size
p a.take((1 << 63) - 1).size
p a.drop(1 << 62)
p [1, 2, 3].each_slice((1 << 62)).to_a

# every count argument clamps in long width, never after a 32-bit narrowing
b = [1, 2, 3, 4, 5]
p [b.first(1 << 62), b.last(1 << 62), b.take(1 << 62), b.drop(1 << 62)]
c1 = b.dup; p [c1.pop(1 << 62), c1]
c2 = b.dup; p [c2.shift(1 << 62), c2]
p [b.min(99), b.max(99), b.sample(0)]
p [b.each_slice(1 << 62).to_a, b.each_cons(1 << 62).to_a]
p [(1..5).first(99), (1..5).min(99), (1..5).max(99)]
p (1..).first(4)
p [b.each_slice(2).to_a, b.each_cons(2).to_a, [].each_slice(3).to_a, [].each_cons(3).to_a]
p [b.first(0), b.last(0), b.take(0), b.drop(99), b.min(2), b.max(2)]
p [(1..5).first(2), (1..5).min(2), (1..5).max(2), (1...5).first(99)]
