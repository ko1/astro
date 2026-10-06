# Array#each's hoisted yield: per-element bindings and closures, break / next / redo / return, raise and its
# backtrace, nested each, an array mutated while iterated, multi-param blocks (splat and #to_ary), forwarded
# lambda / Method / Symbol procs, a block that captures its frame, and self inside the block
class ToA; def to_ary = [:p, :q]; end
class Holder
  def initialize = @v = :hv
  def run(a) = a.each { |x| return [:ret, x, @v] if x == 2 }
  def selfs(a) = a.map { |_| self.class }.first
end
2.times do |r|
  GC.start if r == 1
  procs = []; [1, 2, 3].each { |x| procs << -> { x } }; p procs.map(&:call)
  p [1, 2, 3].each { |x| break x * 10 if x == 2 }
  out = []; [1, 2, 3, 4].each { |x| next if x.even?; out << x }; p out
  tries = 0; out = []; [1, 2].each { |x| tries += 1; redo if tries == 1; out << x }; p [out, tries]
  p Holder.new.run([1, 2, 3])
  begin
    [1, 2].each { |x| raise "boom#{x}" if x == 2 }
  rescue => e
    p [e.message, e.backtrace[0..1].map { |l| l[/'.*'/] }]
  end
  p [[1, 2], [3]].each.with_index.map { |(a, b), i| [a, b, i] }
  m = []; [[1, 2], [3, 4]].each { |a| a.each { |b| m << b } }; p m
  arr = [1, 2, 3]; seen = []; arr.each { |x| seen << x; arr << 9 if x == 1 && arr.size < 5 }; p seen
  arr = [1, 2, 3, 4]; seen = []; arr.each { |x| seen << x; arr.pop }; p seen
  pairs = []; [[1, 2], [3, 4], 5, ToA.new].each { |a, b| pairs << [a, b] }; p pairs
  l = ->(x) { x * 2 }; acc = []; [1, 2].each { |x| acc << l.(x) }; p acc
  p [1, 2].each(&l), [%w[a], %w[b]].each(&:first)
  def dbl(x) = $dbl << x * 2
  $dbl = []; [3, 4].each(&method(:dbl)); p $dbl
  sum = 0; [1, 2, 3].each { |x| sum += x; binding.local_variable_get(:x) }; p sum
  p Holder.new.selfs([1])
  e = [1, 2].each; p e.next, e.next
  lz = []; (1..3).to_a.each { |x; y| y = x * x; lz << y }; p lz
end
# map / times / Hash#each through the same hoisted yield
3.times do |r|
  GC.start if r == 2
  p [1, [2, 3], 4].map { |x| x }, [[1, 2], [3, 4]].map { |a, b| [b, a] }, 3.times.map { |i| i * i }
  t = []; 4.times { |i| next if i == 1; t << i; break if i == 2 }; p t
  h = { a: 1, b: 2 }; kv = []; h.each { |k, v| kv << [v, k] }; h.each { |pr| kv << pr }; h.each { |k, v, z| kv << z }; p kv
  q = []; h.each { |k, v| q << -> { [k, v] } }; p q.map(&:call)
  begin; { x: 1 }.each { |k, v| raise "in #{k}" }; rescue => e; p [e.message, e.backtrace[0..1].map { |l| l[/'.*'/] }]; end
  p [3, 4].map.with_index { |x, i| x * i }
end
# any? / all? / none? / select / reject through the hoisted yield
2.times do |r|
  GC.start if r == 1
  a = [1, 2, 3, nil]
  p a.any? { |x| x == 2 }, a.all? { |x| x }, a.none? { |x| x == 9 }, a.any?(Integer), a.any?, [[1, 2]].any? { |x, y| y == 2 }
  p a.select { |x| x && x.odd? }, a.reject(&:nil?), [[1, 2], [3, 4]].select { |x, y| y > 2 }, a.select.with_index { |x, i| i.even? }
  cl = []; [1, 2].select { |x| cl << -> { x }; true }; p cl.map(&:call)
  p [1, 2, 3].any? { |x| break :early if x == 2; false }
end
# each_with_index / each_with_object / find
2.times do |r|
  GC.start if r == 1
  o = []; %w[a b].each_with_index { |x, i| o << [x, i] }; %w[c].each_with_index { |x| o << x }; [[1, 2]].each_with_index { |(a, b), i| o << [a, b, i] }; p o
  p %w[a b].each_with_object({}) { |x, h| h[x] = x.upcase }, [[1, 2]].each_with_object([]) { |(a, b), m| m << b }
  p [1, 2, 3].find { |x| x > 1 }, [1].find { |x| x > 5 }, [[1, 2], [3, 4]].find { |a, b| b == 4 }, [1].find(-> { :none }) { false }
end
