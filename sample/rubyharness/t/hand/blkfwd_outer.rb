# `def m(&blk)` whose blk is only passed on as `&blk` — from inside a block too (ActiveModel's fetch_value,
# TypeMap#fetch, Template#render): no Proc is made; the forward reads the method's block through the env chain.
class Inner
  def each2 = (yield 1; yield 2)
  def run = yield
  def value(x) = block_given? ? yield(x) : [:noblk, x]
end
class Fw
  def initialize = @in = Inner.new
  def fetch(x, &blk)                 # receiver call from a block
    r = []
    @in.each2 { |i| r << @in.value(x + i, &blk) }
    r
  end
  def fetch_self(x, &blk)            # implicit-self call from a block
    @in.run { helper(x, &blk) }
  end
  def helper(x) = block_given? ? yield(x) : [:nohelper, x]
  def deep(x, &blk)                  # two blocks deep
    @in.run { @in.run { @in.value(x, &blk) } }
  end
  def mixed(x, &blk)                 # yield + block_given? next to a forward in a block
    a = block_given? ? yield(x) : :none
    b = @in.run { @in.value(x * 10, &blk) }
    [a, b]
  end
  def escape(&blk)                   # the forwarding block outlives the method
    proc { |x| @in.value(x, &blk) }
  end
  def escape_lambda(&blk) = ->(x) { helper(x, &blk) }
  def brk(&blk) = @in.run { @in.value(5, &blk) }
  def threaded(x, &blk) = Thread.new { @in.value(x, &blk) }.value
  def kwmix(x, &blk) = @in.run { @in.value(x, &blk) }
end
f = Fw.new
3.times do |k|
  p f.fetch(k) { |v| v * 100 }
  p f.fetch(k)
  p f.fetch(k, &:succ)
  p f.fetch(k, &->(v) { -v })
  p f.fetch_self(k) { |v| [:got, v] }
  p f.fetch_self(k)
  p f.deep(k) { |v| v + 0.5 }
  p f.deep(k)
  p f.mixed(k) { |v| v.to_s }
  p f.mixed(k)
  e = f.escape { |v| [:esc, v] }
  p e.call(k), e.call(k + 1)
  p f.escape.call(k)
  l = f.escape_lambda { |v| [:lam, v] }
  p l.call(k)
  p f.escape_lambda.call(k)
  p(f.brk { |v| break [:broke, v] })
  p f.threaded(k) { |v| [:thr, v] }
  p f.threaded(k)
  pr = proc { |v| [:pr, v] }
  p f.kwmix(k, &pr)
  p f.kwmix(k, &nil)
end
# forwarded from a block run on another thread: the method's frame is not on that stack — a Proc, whose break is orphaned
def t_rescue = (yield rescue [$!.class, $!.message])
def yield_in_thread(&b) = Thread.new { t_rescue { [1].each(&b) } }.value
p yield_in_thread { break :y }
p yield_in_thread { |x| x + 1 }
def fwd_after(&b) = proc { [1].map(&b) }
pa = fwd_after { |x| x * 7 }
p pa.call
pb = fwd_after { break :late }
p((pb.call rescue [$!.class, $!.message]))
