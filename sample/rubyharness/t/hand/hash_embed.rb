# Small hashes keep their pairs in the object: growing past the inline room, deleting, rehashing, default
# procs, compare_by_identity, keyword arguments, GC moving them, and nesting
def kw(**o) = o
3.times do |r|
  GC.start if r == 1
  h = {}
  8.times { |i| h["k#{i}"] = i; p h.size, h.to_a.last if [3, 4, 5].include?(i) }
  p h, h.delete("k2"), h.key?("k2"), h.select { |_, v| v.odd? }, h.min_by { |_, v| -v }
  g = { a: 1, b: 2 }.freeze; p g, g.frozen?, (begin; g[:c] = 3; rescue => ex; ex.class; end), g.merge(c: 3, d: 4, e: 5)
  d = Hash.new { |hh, k| hh[k] = k.to_s * 2 }; 6.times { |i| d[i] }; p d, d.default_proc.arity
  c = {}.compare_by_identity; k = "x"; c[k] = 1; c["x".dup] = 2; GC.start; p c.size, c[k]
  p kw(a: 1), kw(a: 1, b: 2, c: 3, d: 4, e: 5), kw(**{ z: 26 }, y: 25)
  n = { l: { m: { n: [1, { o: 2 }] } } }; GC.start; p n.dig(:l, :m, :n, 1, :o), n.to_a.flatten(1)
  t = { 1 => :a, 2 => :b, 3 => :c, 4 => :d }; t[5] = :e; t.shift; t.reject! { |kk, _| kk.even? }; p t, t.invert
  s = { "b" => 2, "a" => 1 }; s.rehash; p s.sort.to_h, s.transform_values { _1 * 10 }, s.to_a.transpose
  e = {}; e.store(:only, 1); e.clear; e[:again] = 2; p e, e.compact, e.any?, e.sum { |_, v| v }
end
