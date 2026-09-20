# CRuby には Kernel#block_given? の実体があり、ostruct が
# `alias_method :block_given!, :block_given?` する。koruby では
# block_given? は通常パースノードなので、呼び出し元のフレームを見る
# builtin も用意する。
# 既知の近似: 呼び出し元のメソッドが block_given?/yield を「書いていない」と
# ブロックの記録枠 (trio) が無いので false を返す。
class Object
  alias_method :block_given!, :block_given?
  private :block_given!
end
def uses_syntax; block_given? ? [:yes, block_given!] : [:no, block_given!]; end
p uses_syntax { }
p uses_syntax
p Object.private_method_defined?(:block_given?)
p Object.private_method_defined?(:iterator?)
