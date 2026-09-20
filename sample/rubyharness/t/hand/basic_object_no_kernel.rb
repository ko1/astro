# BasicObject の子孫は Kernel のグローバル関数 (puts/p/raise/format...) を
# 持たない。builder の XmlBase は `xml.p` が method_missing に落ちることに
# 依存している。
class BO < BasicObject
  def method_missing(sym, *args, &blk) = [:mm, sym, args]
  def respond_to_missing?(*) = true
end
b = BO.new
p b.p("x")
p b.puts("y")
p b.format("%d", 1)
p b.foo(1, 2)
p BasicObject.instance_methods(false).sort
p BO.instance_methods.sort
# 普通のオブジェクトは今まで通り
class Reg; end
p Reg.new.respond_to?(:puts, true)
p Reg.new.send(:format, "%d", 2)
