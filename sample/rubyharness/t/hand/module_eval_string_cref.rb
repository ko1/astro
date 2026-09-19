# class_eval/module_eval(String) はレシーバを cref に積む: 文字列の中で def した
# メソッドも、`module Foo` の中で書いたのと同じように定数を解決する
# (rake の FileUtilsExt がこの形でメソッドを生成し、中から FileUtilsExt を参照する)。
module Outer
  module Inner
    X = 1
    module_eval(<<-EOS, __FILE__, __LINE__ + 1)
      def self.find_inner; Inner; end
      def self.find_x; X; end
      def self.nesting_here; Module.nesting; end
      def self.find_outer; Outer; end
    EOS
  end
  Y = 2
end
p Outer::Inner.find_x
p Outer::Inner.find_inner
p Outer::Inner.find_outer
p Outer::Inner.nesting_here.first

# class_eval でも同じ
class Holder
  Z = 9
  class_eval("def z; Z; end")
end
p Holder.new.z

# レシーバの定数が eval の外側より優先される
W = :top
module Wrap
  W = :inner
  p module_eval("W")
  module_eval("def self.w; W; end")
end
p Wrap.w
