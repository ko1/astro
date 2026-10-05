# `def m(&blk)` whose blk is only passed on (`x.m(&blk)` / `m(&blk)` in the method body) forwards the received block
# without making a Proc: literal blocks, Procs, lambdas, &:sym, Method objects and no block at all; break / next /
# return through one and two forwarding methods; callees that yield, test block_given?, materialize the block
# (lambda?, arity, store it, define_method, instance_exec, Hash.new, Proc.new); methods that read blk otherwise
class Box
  def initialize(h = { a: 1 }) = @h = h
  def get(k, &blk) = @h.fetch(k, &blk)
  def each(&blk) = [1, 2, 3].each(&blk)
  def twice(&blk) = relay(&blk)
  def relay(&blk) = yielder(10, &blk)
  def yielder(x) = block_given? ? yield(x) : :noblk
  def kind(&blk) = inspect_block(&blk)
  def inspect_block(&b) = b ? [b.lambda?, b.arity] : :none
  def keep(&blk) = store(&blk)
  def store(&b) = (@saved = b; :stored)
  def call_saved(*a) = @saved.call(*a)
  def mk(name, &blk) = self.class.send(:define_method, name, &blk)
  def ie(&blk) = instance_exec(&blk)
  def hash_of(&blk) = Hash.new(&blk)
  def proc_of(&blk) = Proc.new(&blk)
  def reads(&blk) = (blk ? blk.call(1) : :none)
  def cond_pass(&blk) = (blk.nil? ? :nil : [1].map(&blk))
end
b = Box.new
3.times do |i|
  GC.start if i == 1
  p b.get(:a) { 0 }, b.get(:z) { |k| "miss #{k}" }, (b.get(:z) rescue $!.class)
  p b.each { |x| x }, b.twice { |x| x + i }, b.twice, b.relay { |x| x * 2 }
  p b.kind { |a, b2| }, b.kind(&->(x) { x }), b.kind(&:upcase), b.kind(&b.method(:yielder)), b.kind
  p b.keep { |x| x * 7 }, b.call_saved(6), b.ie { @h }, b.hash_of { |h, k| h[k] = k.to_s * 2 }[:ab], b.proc_of { 4 }.call
  p b.reads { |x| x + 1 }, b.reads, b.cond_pass { |x| x * 3 }, b.cond_pass
end
b.mk(:dyn) { |x = 2| x * 100 }
p b.dyn, b.dyn(3)
def br = Box.new.each { |x| break x * 10 if x == 2 }
def br2 = Box.new.twice { |x| break :broke }
def nx = Box.new.each { |x| next x * 2 }
def rt = (Box.new.each { |x| return :returned if x == 2 }; :not)
def rt2 = (Box.new.twice { |x| return [:returned2, x] }; :not2)
p br, br2, nx, rt, rt2
sym_each = Box.new.each(&:to_s)
p sym_each
lam = ->(x) { x * 3 }
p Box.new.relay(&lam), Box.new.get(:q, &lam)
class Box; def later(&blk) = keep(&blk); end
bb = Box.new; bb.later { |x| x + 1 }; p bb.call_saved(41)
def chain3(&b) = chain2(&b)
def chain2(&b) = chain1(&b)
def chain1(&b) = [5].map(&b)
p chain3 { |x| x - 1 }, chain3(&:succ), (chain3 rescue $!.class)
