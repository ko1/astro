# Subclassing a builtin and calling `super` from the subclass's own #initialize:
# the object has to be the real payload, and the builtin's #initialize has to
# exist for `super` to reach.
require "set"

class R2 < Range
  attr_reader :tag
  def initialize(a, b, tag)
    super(a, b)
    @tag = tag
  end
end
r = R2.new(1, 5, :x)
p [r.first, r.last, r.tag, r.to_a, r == (1..5), r.class, r.is_a?(Range), r.frozen?]
p({ (1..5) => :v }[R2.new(1, 5, :y)])
class R3 < Range; end
p R3.new(1, 3, true).to_a

class MySet < ::Set; end
MySet.class_eval do
  def initialize(*args, &block)
    @tag = 1
    super
  end
  def tag = @tag
end
s = MySet.new([1, 2, 3, 2])
p [s.to_a.sort, s.class, s.tag]
p MySet.new([1, 2]) { |x| x * 10 }.to_a.sort
p MySet.new.to_a
begin
  MySet.new(42)
rescue ArgumentError => e
  p e.message
end

class MyArr < Array
  def initialize(*args, &block)
    super
  end
end
p [MyArr.new(3, 0), MyArr.new([1, 2]).class]

# operator-named methods called explicitly while forwarding `...`
class Wrap
  def initialize(h) = @h = h
  def [](...)
    @h.[](...)
  end
  def plus(...)
    @h.keys.+(...)
  end
end
w = Wrap.new({a: 1})
p w[:a]
p w.plus([9])
