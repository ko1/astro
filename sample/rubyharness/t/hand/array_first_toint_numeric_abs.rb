# Array#first coerces the count via #to_int; Numeric#abs fallback for subclasses. vs ruby.
class TI; def to_int; 2; end; end
p [1, 2, 3, 4].first(TI.new)
begin; [1].first(Object.new); rescue => e; p e.class; end
p [1, 2, 3].first(2)
class N < Numeric
  attr_reader :v
  def initialize(v); @v = v; end
  def <(o); @v < o; end
  def -@; N.new(-@v); end
  def to_s; "N(#{@v})"; end
end
p N.new(-5).abs.to_s
p N.new(5).abs.to_s
p (-7).abs; p 7.abs; p (-3.5).abs
