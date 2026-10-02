# receiverless m(pos, k: v) with every keyword in declared order (node_call_kw)
def top_kw(x, a:, b: 2) = [:top, x, a, b]
class C
  def initialize = @base = 7
  def run(i)
    [helper(i, k: i * 2), helper2(i, a: 1, b: 2), helper2(i, b: 2, a: 1), helper2(i, a: 3), priv(i, k: 0), top_kw(i, a: 4, b: 5)]
  end
  def helper(x, k: 1) = [x, k, @base]
  def helper2(x, a:, b: 9) = [x, a, b]
  private def priv(x, k: 1) = (y = x + k; [:priv, y])
  def deep(n, acc: 0) = n == 0 ? acc : deep(n - 1, acc: acc + n)
  def bad(x) = helper(x, nope: 1)
end
c = C.new
3.times { |i| p c.run(i) }
p c.deep(500, acc: 0)
begin
  c.bad(1)
rescue ArgumentError => e
  p e.message
end
p top_kw(1, a: 2, b: 3)
# redefinition at the same site
class C; def helper(x, k: 1) = [:redef, x, k]; end
p c.run(1).first
