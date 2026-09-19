# koruby は「subclass か singleton か」を 1 つの override テーブルで持つので、
# singleton を生やすと subclass が隠れる。#dup は singleton を落とすが
# **その下の本当のクラスは残す** (CRuby)。
# (rack の Rack::Headers のテストが `def @h.foo` のあと dup している)
class DownHash < Hash
  def []=(k, v); super(k.is_a?(String) ? k.downcase : k, v); end
  def [](k); super(k.is_a?(String) ? k.downcase : k); end
end
h = DownHash.new
def h.foo; 1; end
h2 = h.dup
h3 = h.clone
p h2.class, h3.class
p(begin; h2.foo; rescue NoMethodError; :nomethod; end)
p h3.foo
h2['A'] = '2'
h3['B'] = '3'
p h2.keys, h3.keys
p h2['a'], h3['b']

# merge も receiver のサブクラスを返す
p DownHash.new.merge("K" => 1).class
m = DownHash.new
def m.qux; 9; end
p m.merge("K" => 1).class

# 素の builtin は今までどおり
g = {}
def g.bar; 2; end
p g.dup.class
p(begin; g.dup.bar; rescue NoMethodError; :nm2; end)
p g.clone.bar
a = []
def a.baz; 3; end
p a.dup.class
p(begin; a.dup.baz; rescue NoMethodError; :nm3; end)
p a.clone.baz
s = +"x"
def s.qux; 4; end
p s.dup.class, s.clone.qux

# Hash.new(default) と block は片方だけ — サブクラスでも同じ
p(begin; Hash.new('1') { |hash, k| k }; rescue ArgumentError; :ae; end)
p(begin; DownHash.new('1') { |hash, k| k }; rescue ArgumentError; :ae2; end)
p DownHash.new('1').default
p DownHash.new { |hash, k| :blk }[:missing]
