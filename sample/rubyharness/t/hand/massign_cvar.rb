# 多重代入のターゲットにクラス変数 (rubygems の specification.rb が使う形)
class T
  @@a = nil; @@b = nil
  def self.set; @@a, @@b = 1, 2; end
  def self.get; [@@a, @@b]; end
  def self.set_splat; @@a, *@@rest = 1, 2, 3; [@@a, @@rest]; end
  def self.set_mixed; local, @@b = :l, :cv; [local, @@b]; end
end
T.set; p T.get
p T.set_splat
p T.set_mixed
class U
  @@x = 0
  def self.swap; @@x, @@y = 9, 8; [@@x, @@y]; end
end
p U.swap

# 重複した `_` を含むブロック引数: 位置ごとに正しいスロットへ束ね、
# 重複名は FIRST occurrence が勝つ (CRuby)
p proc { |_, _| _ }.call(1, 2)
p proc { |a, _, _, b| [a, b] }.call(1, 2, 3, 4)
p lambda { |t, _, _, v, c| [t, v, c] }.call(1, 2, 3, 4, 5)
p proc { |_, x, _| x }.call(:a, :b, :c)
p proc { |_, _, _| _ }.call(7, 8, 9)
p [[1,2,3,4]].map { |a, _, _, d| [a, d] }
