# send / __send__ / public_send は dispatch の特例だが、CRuby では
# ちゃんとした public メソッド。racc と sequel が alias する。
class Object
  alias __send2 __send__
  alias_method :send2, :send
end
p 1.__send2(:+, 2), 1.send2(:+, 3)
p Object.method_defined?(:send), Object.public_method_defined?(:__send__),
  Object.private_method_defined?(:public_send), Object.method_defined?(:public_send)
p Object.instance_method(:send).bind(1).call(:+, 4)
m = Object.instance_method(:public_send)
p m.owner, m.name
def taking(&b) = b.call
class Object; alias_method :psend, :public_send; end
p [1, 2].psend(:map) { |x| x * 2 }
