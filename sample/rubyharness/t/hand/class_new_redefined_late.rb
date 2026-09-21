class F
  def initialize(a = nil) = @a = a
  def a = @a
end
p F.new(1).a                       # warms the plain-class .new path
class << F
  def new(*args) = "stubbed#{args.inspect}"
end
p F.new(2)
p F.new(2)

class G
  def initialize(x = 0) = @x = x
  def x = @x
end
p G.new(3).x
G.singleton_class.send(:define_method, :new) { |*a| [:dm, a] }
p G.new(4)
G.singleton_class.send(:remove_method, :new)
p G.new(5).x                       # back to the default allocator

class H
  def initialize(y = 0) = @y = y
  def y = @y
end
p H.new(6).y
module NewMod
  def new(*a) = [:mod, a]
end
H.extend(NewMod)                   # a module can supply .new too
p H.new(7)

# an alias of the original new still allocates
class I
  def initialize(z = 0) = @z = z
  def z = @z
end
p I.new(8).z
class << I
  alias_method :orig_new, :new
  def new(*a) = orig_new(*a)
end
p I.new(9).z
