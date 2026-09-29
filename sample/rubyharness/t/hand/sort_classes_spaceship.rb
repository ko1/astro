# Array#sort / min / max of classes: a <=> on a superclass's singleton (Rails::Railtie) and Module#<=>
class R
  class << self
    attr_accessor :idx
    def <=>(o) = idx <=> o.idx
  end
end
class X < R; self.idx = 2; end
class Y < R; self.idx = 1; end
class Z < R; self.idx = 3; end
p [X, Y, Z].sort, [X, Y, Z].max, [X, Y, Z].sort_by(&:idx), [X, Y, Z].min
p [X, Y, Z].minmax, [X, Y, Z].sort.reverse, [Z, Y, X].sort!, [String, Integer, Comparable].sort rescue p $!
p [Integer, Numeric, Comparable].sort, [A = Class.new, Class.new(A)].sort.size
