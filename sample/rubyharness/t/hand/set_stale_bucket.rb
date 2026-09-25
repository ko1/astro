# Set buckets members by their insertion-time #hash (CRuby): a member mutated
# after insertion is found neither by its old nor its new value.
a = [1]; s = Set.new([a, [2]])
a << 9
p s.include?([1,9]), s.include?([1]), s.include?(a), s.size
s << [1,9]; p s.size
s.reset; p s.size, s.include?([1,9])
h = {x: 1}; s2 = Set[h]; h[:y] = 2; p s2.include?({x: 1, y: 2}), s2.include?(h)
str = +"ab"; s3 = Set[str]; p s3.include?("ab")  # strings are dup-frozen? 
p Set[1,2,3] & [3,2], Set[1,2,3] & Set[3,2], Set[1,2,3] ^ [3,4], Set[1,2] | [3,1], Set[1,2,3] - [2]
s = Set[1]; s.delete(1.0); p s; p Set[1].include?(1.0)
f = Set[1].freeze; begin; f << 2; rescue => e; p e.class; end
begin; f.delete(1); rescue => e; p e.class; end
d = Set[1].compare_by_identity; p d.dup.compare_by_identity?
class K; attr_reader :v; def initialize(v)=@v=v; def hash = @v.hash; def eql?(o) = o.is_a?(K) && o.v == v; end
s = Set[K.new(1), K.new(2)]; p s.include?(K.new(1)), s.size, (s << K.new(1)).size, s.delete?(K.new(2)).size
p Set[[1],[2]].to_a.equal?(Set[[1]].to_a)
x = Set[3,1,2]; p x.to_a, x.first, x.sort
a = [1]; s = Set[a, [2], [3]]; a << 2
s.delete_if { |x| x.size == 2 }; p s
a = [1]; s = Set[a, [2]]; a << 2; s.clear; p s, s.size; s << [1]; p s
s = Set[1,2,3,4]; s.select!(&:even?); p s; s.map! { |x| x * 10 }; p s
s = Set[1,2,3]; p s.reject! { false }, s.keep_if { |x| x > 1 }
f = Set[1].freeze; %i[clear reset].each { |m| begin; f.send(m); rescue => e; p [m, e.class]; end }
s = Set.new(1..2000); s.subtract(1..1990); p s.size, s.include?(1995), s.to_a.last(3)
s = Set[1, 2] ; s.merge([3], Set[4]); p s, s ^ Set[1, 9], s <= Set[1,2,3,4,5], s <=> Set[1,2,3,4], s == Set[4,3,2,1]
h = Hash.new; 20.times { |i| h[i] = i }; 18.times { |i| h.delete(i) }; p h, h[19], h.key?(3)
e = Set["a", "b"].compare_by_identity; p e.include?("a"), e.dup.compare_by_identity?, (e | ["a"]).size
p Set.new((0...3000).to_a).size, Set.new((0...3000).map(&:to_s)).include?("2999")
s = Set[1, 2, 3]; a = s.to_a; a << 4; p s, a.equal?(s.to_a)
x = Set[+"ab"]; p x.first.frozen?, Set[+"ab"].compare_by_identity.first.frozen?
