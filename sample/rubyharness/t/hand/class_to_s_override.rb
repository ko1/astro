# クラスが self.to_s を上書きしたら、出力側 (Array#join / "%s" / 文字列補間 /
# print) はそれを使う。#inspect は CRuby では別メソッドなので上書きされない。
# (minitest の Spec クラスは自分で名前を名乗り、入れ子の名前を Array#join で組む)
c = Class.new do
  def self.to_s; "CUSTOM"; end
end
p [c, "x"].join("::")
p "#{c}"
p c.to_s
p "%s" % [c]
p format("%s/%s", c, "tail")
m = Module.new do
  def self.to_s; "MODCUSTOM"; end
end
p [m, "y"].join("-")
class Named
  def self.to_s; "NAMED_TS"; end
end
p [Named, "z"].join("|")
p [1, :a, "b", Named].join(",")
p "#{Named}"
# inspect は上書きされない (Module#inspect は別メソッド)
p Named.inspect
p [Named].inspect
# 素のクラスは今までどおり
class Plain; end
p [Plain, "q"].join(":")
p "#{Plain}"
p Plain.inspect
# インスタンス側
o = Object.new
def o.to_s; "OBJ"; end
p [o, "w"].join("+")
p "%s" % [o]
