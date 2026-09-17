# Kernel#p の複数引数、class body の可視性スコープ、Kernel#local_variables、
# lambda を通り抜ける return、respond_to_missing? 越しの public_method。

# p は 1 引数なら本人、複数なら Array、0 個なら nil
x = p(1, 2)
p x
p(p(3))
p p()

# class << self の private は外側の `def self.x` に漏れない
class VisM
  def self.ichi; end
  class << self
    def san; end
  end
  private
  def self.shi; end
  class << self
    def roku; end
    private
    def shichi; end
  end
  protected
  def self.hachi; end
  class << self
    def juu; end
    protected
    def juu_ichi; end
  end
  public
  def self.juu_ni; end
end
p VisM.methods(false).sort

# 再オープンした body は public から始まり、終わると元に戻る
class VisM2
  private
  def a; end
end
class VisM2
  def b; end
end
p [VisM2.private_instance_methods(false), VisM2.public_instance_methods(false).sort]

# Kernel#local_variables — 自分のフレーム + 外側スコープ
lv_a = 1
def lv_m; lv_b = 2; local_variables; end
p lv_m
p local_variables.include?(:lv_a)
[3].each { |lv_x| lv_y = 1; p local_variables.sort }
p Kernel.private_instance_methods(false).include?(:local_variables)
p Kernel.public_methods(false).include?(:local_variables)

# lambda は「自分の body が出した return」だけ飲む
class LamT
  def outer = inner
  def mp(&b) = b
  def inner
    b = mp { return :good }
    (-> x { x.call }).call(b)
    :bad
  end
end
p LamT.new.outer
p (-> { [1].each { return :in_lambda }; :nope }).call
p (lambda { return :direct }).call
def lam_m
  pr = -> { return :lam }
  [pr.call, :after]
end
p lam_m

# respond_to_missing? は非 public なメソッドについても呼ばれる
class RtmT
  def respond_to_missing?(m, priv = false) = (m == :pub) || (priv && m == :prv)
  def method_missing(m, *a) = "mm:#{m}"
  private def real_priv; end
end
r = RtmT.new
p [r.respond_to?(:pub), r.respond_to?(:prv), r.respond_to?(:prv, true)]
p [r.respond_to?(:real_priv), r.respond_to?(:real_priv, true)]
p r.method(:pub).call
p r.public_method(:pub).call
begin; r.public_method(:prv); rescue NameError => e; p e.class; end
