# include は既にチェーンにあるモジュールを入れ直さない (深い方の位置が残る)。
# prepend は別: 既に include されていても前に入るので重複したまま (CRuby)。
module Mm; def who = "M"; end
class Aa; include Mm; end
class Bb < Aa; prepend Mm; end
p Bb.ancestors
class Cc; prepend Mm; include Mm; end
p Cc.ancestors
module Inner; end
module Outer2; include Inner; end
class Dd; include Outer2; end
class Ee < Dd; include Outer2; end
p Ee.ancestors
module Deep; include Inner; end
class Ff; include Outer2; include Deep; end
p Ff.ancestors
# 重複があると super が自分自身を見つけて無限再帰していた (dry-types)
EQ = Module.new do
  define_method(:hash) { @h ||= 7 }
  define_method(:freeze) { hash; super() }
end
module Comp3
  include EQ
  def self.included(base) = base.const_set("Sub", ::Class.new(base))
end
class SumZ; include Comp3; end
p SumZ.ancestors.size == SumZ.ancestors.uniq.size
p SumZ::Sub.ancestors.size == SumZ::Sub.ancestors.uniq.size
p SumZ::Sub.new.freeze.frozen?
p SumZ::Sub.instance_method(:freeze).super_method.owner
# Module#singleton_class? — attached_object は Class メソッドなので、
# 素の Module では NameError ではなく false を返す (pry の safe_send が
# Module.instance_method(:singleton_class?) を bind する)。
m = Module.new
p m.singleton_class?
p Module.instance_method(:singleton_class?).bind(m).call
c = Class.new
p c.singleton_class.singleton_class?, c.singleton_class?, Integer.singleton_class?
