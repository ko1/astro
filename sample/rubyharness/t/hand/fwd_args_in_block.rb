# `...` 転送は def m(...) の body の中の**ブロック**からも使える。
# (rack の Rack::Builder#use が `@use << proc { middleware.new(app, ...) }` を使う)
def target(*a, **k, &b)
  [a, k, b ? b.call : nil]
end
def in_block(...)
  proc { target(...) }.call
end
p in_block(1, 2, x: 3)
p(in_block(4) { :blk })

def nested(...)
  [1].map { [2].map { target(...) } }
end
p nested(9, y: 8)

def direct(...)
  target(...)
end
p direct(5, z: 6)
p(direct(5) { :d })

# レシーバ付き
class T
  def self.go(*a, **k, &b); [a, k, b ? b.call : nil]; end
end
def recv_in_block(...)
  proc { T.go(...) }.call
end
p recv_in_block(1, q: 2)
p(recv_in_block(3) { :r })

# 先頭に固定引数を足す形
def with_leading(...)
  proc { target(:first, ...) }.call
end
p with_leading(7, m: 8)

# インスタンスメソッド + ivar 経由
class C
  def m(...)
    @r = proc { helper(...) }
    @r.call
  end
  def helper(*a, **k, &b); [a, k, b ? b.call : nil]; end
end
p C.new.m(7, w: 1)
p(C.new.m(7) { :c })

# 二重ブロック越し
def two(...)
  proc { proc { target(...) }.call }.call
end
p two(1, n: 2)
