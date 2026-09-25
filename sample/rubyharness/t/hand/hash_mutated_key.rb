# A key mutated after insertion hashes differently, so the entry becomes
# unreachable (CRuby buckets by #hash at store time).
memo = {}
o = { a: 1, only: ["x"] }
memo[o] ||= "PHASE1"
o.delete(:only)
p memo[o]
p memo.size
memo[o] ||= "PHASE2"
p memo.size, memo.values.sort

a = [1, 2]
h = { a => :v }
p h[a]
a << 3
p h[a]
p h[[1, 2]]
a.pop
p h[a]

# a key that never changes is still found
k = { x: 1 }
g = { k => :ok }
p g[k], g[{ x: 1 }]

# user object whose #hash follows mutable state
class MK
  attr_accessor :n
  def initialize(n) = @n = n
  def hash = @n.hash
  def eql?(o) = o.is_a?(MK) && o.n == @n
end
m = MK.new(1)
t = { m => :one }
p t[m]
m.n = 2
p t[m]
p t.size
t[m] = :two
p t.size, t.values.sort_by(&:to_s)
