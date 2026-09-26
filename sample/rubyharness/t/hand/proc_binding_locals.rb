# Proc#binding sees the locals around the block (also for a block that reads none, while its frame lives).
x = 1
pr = proc { x }
p((eval("x", pr.binding) rescue $!.class))
def mk(y) = proc { y + 1 }
q = mk(10)
p [(eval("y", q.binding) rescue $!.class), eval("y * 3", q.binding)]
b = pr.binding
eval("x = 5", b); p x
eval("z = 7", b); p b.local_variable_get(:z), b.local_variables.sort, (b.local_variable_defined?(:x))
lam = ->(a) { a }; p lam.binding.local_variables.include?(:x)
p b.receiver.equal?(self), b.source_location.class
def check(src, &blk) = eval(src, blk.binding)
def user
  list = [3, 4]
  r = check("i = 0; list.each_with_index { |x, i| }; [list.sum, i]") {}
  [r, local_variables.sort]
end
p user
class K; def run = [1].map { |v| check("v + @w.to_i") {} }; def initialize = @w = 2; end
p K.new.run
def later = proc {}
pl = later
p [(eval("1 + 1", pl.binding) rescue $!.class), pl.binding.local_variables]
p proc {}.binding.source_location[1]
class T
  def check(src, &blk) = eval(src, blk.binding)
  def self.it(name, &b) = define_method(name, &b)
  it(:t1) { list = [1]; check("list.size") {} }
  describe = proc { it(:t2) { list = [2, 3]; check("list.size") {} } }
  describe.call
end
p T.new.t1, T.new.t2
