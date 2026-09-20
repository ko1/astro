# attr_reader / attr_writer / attr_accessor はメソッド呼び出し。クラス (が extend した
# モジュール) がそれを定義していたら、そちらが走らないといけない。koruby は
# parse 時に node_attr へ落としていて、override を素通りしていた
# (equatable が attr_reader を上書きして属性名を集める)。
module E
  def self.included(base)
    base.extend(ClassMethods)
    base.instance_variable_set("@attrs", [])
  end
  module ClassMethods
    attr_reader :attrs
    def attr_reader(*args)  = (attrs.concat(args); super)
    def attr_writer(*args)  = (attrs.concat(args.map { |a| :"#{a}=" }); super)
    def attr_accessor(*args)= (attrs.concat(args.map { |a| :"#{a}*" }); super)
    def inherited(sub) = (super; sub.instance_variable_set(:@attrs, attrs.dup))
  end
end
class K
  include E
  attr_reader :a
  attr_writer :b
  attr_accessor :c
  def initialize = (@a = 1; @b = 2; @c = 3)
end
p K.attrs
o = K.new
p [o.a, o.c]
o.b = 9
p o.instance_variable_get(:@b)
p Class.new(K).attrs
# override が無いクラスは今までどおり
class P2
  attr_accessor :x
  attr_reader :y
  attr_writer :z
end
q = P2.new; q.x = 1; q.z = 3
p [q.x, q.y, q.instance_variable_get(:@z)]
p P2.instance_methods(false).sort
# private attr_reader も維持される
class R2
  private attr_reader :s
  def initialize = (@s = 7)
  def get = s
end
p R2.new.get
begin; R2.new.s; rescue NoMethodError => e; p e.class.to_s; end
