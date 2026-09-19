# ブロックの中の yield は、外側メソッドのブロックに届かなければならない。
# 途中のブロックが Proc として reify される (&b で受けて b.call する) と、その
# フレームの EP セルは親ではなく「自分の env」を指すようになるので、env chain を
# 1 段ずつ辿るだけでは 1 レベル足りなくなる。
# test-unit の assert_raises が _wrap_assertion(&block){ block.call } を
# 二重に通すのでこれを踏む。
def wrap(&b)
  b.call
end
def ab
  wrap do
    yield
  end
end
def outer
  wrap do
    ab do
      yield
    end
  end
end
p(outer { :found })

# 3 段
def three
  wrap do
    wrap do
      ab do
        yield
      end
    end
  end
end
p(three { :deep })

# ブロック引数と混ぜる
def mixed(&blk)
  wrap do
    ab do
      [yield, blk.call]
    end
  end
end
p(mixed { :both })

# block_given? も同じ経路
def bg
  wrap do
    ab do
      block_given?
    end
  end
end
p bg {}
p bg
