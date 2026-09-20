# __callee__ は「呼ばれた名前」。alias / alias_method / define_method(name, unbound)
# のコピーは名前を付け替えるので、定義時の名前を焼いてはいけない。
# (__method__ は定義時の名前のままでよい。)
# cgi の TagMaker が define_method(element.downcase, instance_method(:nn_element_def))
# で全タグを作り、本体が __callee__ を読む。
class K
  def m = [__method__, __callee__]
  alias n m
  alias_method :o, :m
  define_method(:q, instance_method(:m))
end
p K.new.m, K.new.n, K.new.o, K.new.q

module TM
  def gen(x = nil) = [__callee__, x]
end
class L
  include TM
  instance_method(:gen).tap { |mm| define_method(:aa, mm); define_method(:bb, mm) }
end
p L.new.aa(1), L.new.bb(2)

# ブロックの中、define_method の中、method_missing 経由
class M2
  def outer = [1].map { [__method__, __callee__] }
  alias outer2 outer
  define_method(:dm) { [__method__, __callee__] }
  def respond_to_missing?(n, p = false) = true
  def method_missing(n, *a) = [__method__, __callee__]
end
p M2.new.outer, M2.new.outer2, M2.new.dm, M2.new.zzz
p method(:puts).name
def top = __callee__
alias top2 top
p top, top2
