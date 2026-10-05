# Small arrays keep their elements in the object: growing past the inline room, freezing (shares a header bit
# with other types), shrinking, in-place edits, GC moving them, and nesting
def mk(n) = Array.new(n) { |i| "s#{i}" }
3.times do |r|
  GC.start if r == 1
  a = []
  10.times { |i| a << i; p a.size, a.last if i == 2 || i == 3 || i == 6 || i == 7 }
  p a
  b = [1, 2, 3].freeze
  p b, b.frozen?, (b << 4 rescue $!.class), b.dup << 4
  c = mk(6); c.reverse!; c.rotate!(2); c.shuffle!(random: Random.new(1)); p c.sort
  d = mk(3); d[1] = :x; d.unshift(:u); d.insert(2, :i); p d, d.compact!, d.delete(:x), d
  e = [mk(2), [mk(1)], []]; GC.start; p e, e.flatten, e.map(&:size)
  f = [3, 1, 2]; f.sort!; f.pop; f.push(9, 8, 7, 6, 5); p f, f.slice!(1, 3), f
  g = %w[a b]; g.concat(%w[c d e f g]); g.clear; g << :z; p g
  h = [1, 2, 3, 4, 5, 6]; h << 7; p h, h.each_slice(2).to_a, h.zip(h.reverse).first(2)
  k = [nil, 1, nil]; p k.compact, k.fill(0), k.replace([8] * 9), k
end
