class T
  class << self
    def new(p = nil, &b)
      ($log ||= []) << :T
      p ? super(p) : super("made")
    end
  end
  def initialize(p) = @p = p
  def p = @p
end
class L < T
  class << self
    def new(p = nil, &b)
      ($log ||= []) << :L
      p ? super(p) : super("lazy")
    end
  end
end
p L.new.p
p $log
$log = []
p L.new("x").p
p $log
p T.new.p
class M < L; end
$log = []
p M.new.p
p $log

# a middle class with no own .new still reaches the top one
class Base2
  def self.new(*a) = (($seen ||= []) << :Base2; super)
  def initialize(x = 1) = @x = x
  def x = @x
end
class Mid2 < Base2; end
class Leaf2 < Mid2
  def self.new(*a) = (($seen ||= []) << :Leaf2; super)
end
$seen = []
p Leaf2.new(9).x
p $seen
# define_method-defined .new in the chain
class DM1
  def initialize(v = 0) = @v = v
  def v = @v
end
class DM2 < DM1
  define_singleton_method(:new) { |*a| ($seen2 ||= []) << :DM2; super(*a) }
end
$seen2 = []
p DM2.new(4).v
p $seen2
