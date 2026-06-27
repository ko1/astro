# Method / UnboundMethod #== compares the underlying definition (aliases equal)
p Float.instance_method(:to_int) == Float.instance_method(:to_i)
p Float.instance_method(:magnitude) == Float.instance_method(:abs)
p 1.5.method(:to_int) == 1.5.method(:to_i)
p Float.instance_method(:to_i) == Float.instance_method(:abs)   # different defs
p 1.5.method(:to_i) == 2.5.method(:to_i)                        # different receivers
p Float.instance_method(:to_i) != Float.instance_method(:abs)
p Float.instance_method(:to_i).eql?(Float.instance_method(:to_int))

class AE
  def orig; 1; end
  alias_method :ali, :orig
end
p AE.instance_method(:ali) == AE.instance_method(:orig)
p AE.new.method(:ali) == AE.new.method(:orig)                   # different receivers → false

def foo; end
p method(:foo) == method(:foo)
# Method vs UnboundMethod → not equal
p 1.5.method(:to_i) == Float.instance_method(:to_i)
