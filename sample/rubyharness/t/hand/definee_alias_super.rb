# gem のテストスイートを回して出た 4 件。
# (1) `alias` / `undef` キーワードは default definee を見る (instance_exec の中では
#     レシーバの singleton)。(2) define_method したメソッドを alias 経由で呼んだ
#     ときの `super` は alias 元の名前で探す。(3) Class.new(Module) は合法。

# (1) instance_exec の中の alias
defs = proc do
  def foo = :from_foo
  alias bar foo
end
module M1; end
M1.instance_exec(&defs)
p [M1.foo, M1.bar]

o = Object.new
o.instance_eval do
  def baz = :from_baz
  alias qux baz
end
p [o.baz, o.qux]

# class_eval / module_eval も同じ
class C1; end
C1.class_eval do
  def inst = :inst
  alias inst2 inst
end
p C1.new.inst2

# undef も definee を見る
class C2
  def gone = :gone
end
C2.class_eval { undef gone }
p C2.new.respond_to?(:gone)

# (2) define_method + alias 経由の super
module R2
  def val = :from_R2
end
class C3
  include R2
  define_method(:val) { [:C3, super()] }
  alias_method :val?, :val
end
p [C3.new.val, C3.new.val?]

# def + alias (元から動いていた側)
class C4
  include R2
  def val = [:C4, super]
  alias_method :val2, :val
end
p C4.new.val2

# (3) Class.new(Module)
k = Class.new(Module)
p [k.class, k.superclass]
m = k.new
p [m.class == k, m.is_a?(Module)]
m.send(:define_method, :hi) { :hello }
class Target1; end
Target1.prepend(m)
p Target1.new.hi
begin; Class.new(Class); rescue TypeError => e; p e.message; end
