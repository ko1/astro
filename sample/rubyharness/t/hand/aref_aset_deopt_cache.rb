# recv[i] / recv[i] = v when node_aref / node_aset leave their inline fast path (Hash writes,
# String reads, default procs, frozen receivers, subclasses, a later Ruby redefinition):
# the deopt goes through the node's cache for C methods and behaves as CRuby.  Run under both and diff.
h = Hash.new { |hh, k| hh[k] = k.to_s * 2 }
p h[:ab], h, (h[:x] = 1), h
s = +"hello world"
p s[0], s[1, 3], s[1..2], s["wor"], s[/o./], s[99]
s[0] = "J"; s[-5, 5] = "there"
p s
a = [1, 2]
a[5] = :far
p a
fz = { a: 1 }.freeze
begin; fz[:b] = 2; rescue => e; p e.class; end
begin; "x".freeze[0] = "y"; rescue => e; p e.class; end
class MyH < Hash
  def []=(k, v); super(k, [:wrapped, v]); end
end
m = MyH.new; m[:k] = 1; p m
ci = {}.compare_by_identity
k1 = "k"; ci[k1] = 1; p ci["k"], ci[k1]
def put(hh, k, v) = (hh[k] = v)
g = {}
3.times { |i| put(g, i, i * i) }
p g
class Hash
  alias_method :orig_aset, :[]=
  def []=(k, v); orig_aset(k, v.is_a?(Integer) ? v + 1000 : v); end
end
put(g, :late, 1)
p g[:late]
class String
  alias_method :orig_aref, :[]
  def [](*a) = [:redef, orig_aref(*a)]
end
p "abc"[1]
