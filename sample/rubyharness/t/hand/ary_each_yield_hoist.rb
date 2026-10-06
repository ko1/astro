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
