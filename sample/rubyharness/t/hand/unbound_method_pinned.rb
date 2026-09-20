# UnboundMethod も「取った時点の定義」に束縛される。tilt はコンパイルした
# テンプレートメソッドを捕まえてすぐ remove_method する。
class Host; def zz(x) = [:zz, x]; end
um = Host.instance_method(:zz)
Host.class_eval { remove_method(:zz) }
p um.bind_call(Host.new, 1)
p um.bind(Host.new).call(2)
p Host.method_defined?(:zz)
class Host2; def yy = :old; end
m = Host2.new.method(:yy)
um2 = Host2.instance_method(:yy)
class Host2; def yy = :new; end
p m.call, um2.bind_call(Host2.new)
class Host2; remove_method(:yy); end
p um2.bind_call(Host2.new)
