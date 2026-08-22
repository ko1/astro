def g(a, b = :b, *rest, k: :k, &blk)
  [a, b, rest, k, blk ? blk.call : nil]
end
def f(...)= g(:lead, ...)
p f(1, 2, k: 9)
p f { :blk }
p(f(1))
class P2; def initialize(p) = @p = p
  def base(...) = File.basename(@p, ...)
end
p P2.new("/x/y.rb").base
p P2.new("/x/y.rb").base(".rb")
require "pathname"
p Pathname.new("/a/b.txt").basename.to_s
p Pathname.new("/a/b.txt").basename(".txt").to_s
