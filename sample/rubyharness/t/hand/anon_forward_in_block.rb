# A bare `&` / `*` / `**` may be written inside a BLOCK nested in the
# `def m(&)` / `def m(*)` / `def m(**)` body.  koruby only looked at the
# current frame's synth slot, so `&` raised NotImplementedError and `*` / `**`
# silently forwarded nothing.  simplecov has
#   def synchronize(&) = LOCK_MONITOR.synchronize { with_flock(&) }
def take = yield(7)
def g(*a) = a
def h(**k) = k
def k2(*a, **k, &b) = [a, k, b&.call]

def blk1(&)  = [1].map { take(&) }
def blk2(&)  = [1].map { [2].map { take(&) } }
def blk3(&)  = proc { take(&) }.call
def blk4(&)  = ->{ take(&) }.call
def blk5(&)  = [1].each { |i| return take(&) }
def rest1(*) = [1].map { g(*) }
def rest2(*) = [1].map { [2].map { g(*) } }
def kw1(**)  = [1].map { h(**) }
def kw2(**)  = proc { h(**) }.call
def all1(...) = [1].map { k2(...) }
def mix(*, **, &) = [1].map { k2(*, **, &) }

p blk1 { |v| v * 3 }
p blk2 { |v| v + 1 }
p blk3 { |v| v - 1 }
p blk4 { |v| v * v }
p blk5 { |v| [v] }
p rest1(1, 2)
p rest2(3)
p kw1(a: 1)
p kw2(b: 2)
p all1(1, z: 2) { 5 }
p mix(9, q: 8) { 4 }

# the forwarded block survives a closure that outlives the call
def keep(&) = [1].map { proc { take(&) } }
p keep { |v| v + 100 }.map(&:call)

# inside a class body method and a singleton method
class K
  def s(&) = [1].map { t(&) }
  def t = yield(11)
end
p K.new.s { |v| v * 2 }
o = Object.new
def o.m(&) = [1, 2].map { |x| [x].map(&) }
p o.m { |x| x * 3 }

# a nested def is a scope barrier: the inner `&` must not see the outer slot
def barrier(&)
  [1].map do
    def inner_no_blk = 1
    take(&)
  end
end
p barrier { |v| v }
