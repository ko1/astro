# FrozenError のメッセージ作りはオブジェクト自身の #inspect を走らせる。その
# #inspect がまた同じオブジェクトを書き換えようとすると (rake の FileList は
# 遅延 resolve でこれをやる)、二度目の FrozenError がまた inspect を呼んで
# 無限ループになる。二重に入ったらクラス名だけで組み立てる。
class Lazy
  def initialize; @pending = true; @items = []; end
  def resolve
    if @pending
      @pending = false      # frozen なら FrozenError
      @items << "x"
    end
    self
  end
  def inspect; resolve; @items.inspect; end
  def to_s; inspect; end
  def <<(o); resolve; @items << o; self; end
end
l = Lazy.new
l.freeze
begin
  l << "more"
rescue FrozenError => e
  puts e.class
  puts e.message.start_with?("can't modify frozen Lazy")
end

# 素直なケースは今までどおりメッセージに inspect が入る
s = "abc".freeze
begin
  s << "d"
rescue FrozenError => e
  puts e.message
end
a = [1, 2].freeze
begin
  a << 3
rescue FrozenError => e
  puts e.message
end
