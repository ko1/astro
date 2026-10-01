# A class's own @ivars read / written through an inline cache that remembers the pair's
# position: removal shifts the pairs, the same node runs with an object and a class as self,
# frozen classes, subclasses and instance_variable_set behave as CRuby.  Run under both and diff.
class K
  @a = 1; @b = 2; @c = 3
  def self.a = @a
  def self.b = @b
  def self.c = @c
  def self.setc(v) = (@c = v)
  def self.d = @d
end
p [K.a, K.b, K.c, K.d]
3.times { K.setc(K.c + 1) }
p K.c
K.send(:remove_instance_variable, :@a)
p [K.a, K.b, K.c], K.instance_variables
K.instance_variable_set(:@a, :back)
p [K.a, K.b, K.c], K.instance_variables
module M
  def mv = @v
  def setv(x) = (@v = x)
end
class O; include M; extend M; end
o = O.new
o.setv(:obj); O.setv(:cls)
p [o.mv, O.mv, o.mv, O.mv]
class Sub < K; @c = :sub; end
p [Sub.c, K.c, Sub.b]
K.freeze
begin; K.setc(0); rescue => e; p e.class; end
p K.c
p Class.new { @z = 1; def self.z = @z }.z
