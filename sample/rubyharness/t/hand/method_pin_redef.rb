# Method は取った時点の定義に束縛される (CRuby)。同じ名前を後から定義し直しても
# その Method は古い body を呼ぶ。rspec-mocks の partial double が
# undef_method → define_method(name, saved_method) で元に戻すのにこれを使う。
class K
  def f = "v1"
end
k = K.new
m = k.method(:f)
um = K.instance_method(:f)
class K
  def f = "v2"
end
p [m.call, um.bind(k).call, k.f]
K.send(:undef_method, :f)
p m.call
K.send(:define_method, :f, m)
p k.f
p K.new.f
# 特異メソッド
o = Object.new
def o.g = "g1"
mg = o.method(:g)
def o.g = "g2"
p [mg.call, o.g]
o.singleton_class.send(:remove_method, :g)
o.singleton_class.send(:define_method, :g, mg)
p o.g
# module_function / alias 経由
module MM
  def self.h = "h1"
end
mh = MM.method(:h)
def MM.h = "h2"
p [mh.call, MM.h]
