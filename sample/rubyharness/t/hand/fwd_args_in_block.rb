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

# 転送を使う proc がメソッドの外へ逃げるケース (rack の Builder#use はこれ)。
# 転送スロットは synth local なので、素ではクロージャに捕捉されず SEGV していた。
class Mw
  def initialize(*a, **k, &b); @a = a; @k = k; @b = b; end
  def to_a; [@a, @k, @b ? @b.call : nil]; end
end
class Builder
  def initialize; @use = []; @map = nil; end
  def generate_map(app, m); [:map, app, m]; end
  def use(middleware, ...)
    if @map
      mapping, @map = @map, nil
      @use << proc { |app| generate_map(app, mapping) }
    end
    @use << proc { |app| middleware.new(app, ...) }
    nil
  end
  def to_app; @use.reverse.inject(:INNER) { |a, e| e[a] }; end
end
b = Builder.new
b.use(Mw, 1, x: 2)
p b.to_app.to_a
c = Builder.new
c.use(Mw) { :blk }
p c.to_app.to_a
def escaping(...)
  proc { target(...) }
end
p escaping(5, s: 6).call
p(escaping(7) { :esc }.call)

# 同じメソッドに複数のブロックがあり、`...` を使うのが後ろのほうのケース。
# ブロックは 1 つの env を共有するので、先に閉じたほうがサイズを決めてしまう
# → 捕捉下限は def の時点で立てる必要がある (rack の Builder#use は
#    @map があるときに先へもう 1 つ proc を積む)。
def two_procs(mw, ...)
  m = 1
  pr1 = proc { |x| [x, m] }
  pr2 = proc { |x| mw.new(x, ...) }
  [pr1, pr2]
end
class Mw2
  def initialize(*a, **k, &b); @a = a; @k = k; end
  def to_a; [@a, @k]; end
end
a, b = two_procs(Mw2, 7, r: 8)
p a.call(:A)
p b.call(:B).to_a

def cond_procs(mw, ...)
  if true
    mapping = 5
    pr1 = proc { |x| [x, mapping] }
  end
  pr2 = proc { |x| mw.new(x, ...) }
  [pr1, pr2]
end
c, d = cond_procs(Mw2, 9)
p c.call(:C)
p d.call(:D).to_a
