# Marshal round-trips BasicObject instances (plain ivars and marshal_dump/load) without calling
# their own #respond_to? / #class / #instance_variable_set
class B < BasicObject; def initialize = (@a = 1); end
s = Marshal.dump(B.new); p s.bytesize
x = Marshal.load(s); p ::Kernel.instance_method(:class).bind_call(x), ::Kernel.instance_method(:instance_variable_get).bind_call(x, :@a)
class C < BasicObject; def marshal_dump = [7]; def marshal_load(a) = (@v = a); end
y = Marshal.load(Marshal.dump(C.new)); p ::Kernel.instance_method(:instance_variable_get).bind_call(y, :@v)
