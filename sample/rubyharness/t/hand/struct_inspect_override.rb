# Struct/Data を配列・ハッシュの要素として inspect するとき、koruby は C の
# "#<struct Name f=v>" プリンタに直行していて、クラスが Ruby で定義した #inspect を
# 無視していた (rspec-support の ObjectFormatter は Struct.new(:text) { def inspect = text }
# という形で整形するので、配列の中身が全部 "#<struct Inspec...>" になった)。
# 自己参照する Struct は "#<struct S:...>" (CRuby)。
S = Struct.new(:a, :b)
s = S.new(1, [2, {x: 3}])
p s
p [s]
p({k: s})
p Set[s] rescue p $!.class.to_s
D = Struct.new(:t) do
  def inspect = "<#{t}>"
  def to_s = "TS#{t}"
end
d = D.new("q")
p d, [d], {z: d}, "#{d}"
E = Data.define(:v)
e = E.new(v: 5)
p e, [e]
class F < S
  def inspect = "F!"
end
p F.new(1,2), [F.new(1,2)]
# 再帰
r = S.new(1, nil); r.b = [r]
p [r]
