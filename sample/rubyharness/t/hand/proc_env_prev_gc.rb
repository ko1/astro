# korb_make_proc / korb_make_binding が「もとの PREV リンク」を KorbEnv の alloc を
# 跨いで C ローカルに持っていた。そのリンクが既に materialize 済みの KorbEnv を
# 指していると、alloc 中の GC で動いたあとの古いアドレスが e->prev に焼かれる。
# backtrace のラベル計算 (block in Foo#bar) がそれを辿って SEGV していた。
#
# 再現は「同じフレームを複数のクロージャが別々の深さで掴む」形。STRESS+PURGE で
# 走らせたときだけ落ちるので、ここは値が合っていることだけを見る。
def outer
  a = 1
  mid = proc do          # 深さ 1 のクロージャ: ここで a のフレームが env 化する
    b = 2
    inner = proc { a + b }        # 深さ 2: PREV はもう KorbEnv を指している
    [inner.call, binding.local_variable_get(:b)]
  end
  [mid.call, a]
end
p outer

# Enumerator (= fiber) を跨いでブロックを抜けると backtrace のラベル計算が走る
p (1..3).cycle.first(7)
p (1..3).cycle.take(8)
cnt = 0
(1..3).cycle { |x| cnt += 1; break if cnt >= 7 }
p cnt

def gen
  e = Enumerator.new do |y|
    v = 10
    2.times { |i| y << (v + i) }
  end
  e.take(2)
end
p gen

# binding 経由でも同じ経路を通る
def bnd
  x = 5
  f = proc { y = 6; proc { x + y }.call }
  [f.call, binding.local_variable_get(:x)]
end
p bnd
