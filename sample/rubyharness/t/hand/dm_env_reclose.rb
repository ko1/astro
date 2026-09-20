# 閉じた closure env を二度閉じてはならない。
#
# define_method の本体はフレームの EP セルに proc の env を持つ。その env の loc は
# もう無い「定義時フレーム」のアドレスで、あとから同じアドレスに乗った別のフレームが
# 戻るときに korb_frame_escaped が true になり、captured された値がそのフレームの
# ローカルで上書きされていた (rspec の OutputWrapper#puts が :puts ではなく直前の
# 呼び出しの args を見て TypeError になった)。
#
# 定義側と呼び出し側の深さを振って、同じアドレスに当たる組み合わせを探す。
def build
  Class.new do
    [:m1].each do |name|
      tag = "T"
      define_method(name) do |*args, &blk|
        [name, tag, args]
      end
    end
  end
end
def at_build(n) = n.zero? ? build : at_build(n - 1)
def at_call(n, o) = n.zero? ? o.m1 : at_call(n - 1, o)

bad = []
(0..20).each do |d1|
  k = at_build(d1).new
  (0..20).each do |d2|
    2.times do                      # 2 回目は 1 回目の戻りで壊れた env を読む
      v = at_call(d2, k)
      bad << [d1, d2, v] unless v == [:m1, "T", []]
    end
  end
end
p bad.size
p bad.first(3)
