# #inspect / #hash / #to_s は CRuby では各クラスが「所有」している。
# 本物の pp は `Object.instance_method(:method).bind_call(o, :inspect).owner`
# が Kernel かどうかで self.inspect と pp_object を切り替えるので、
# owner が Kernel だと [:unknown] が [unknown>] になっていた。
[Symbol, String, NilClass, TrueClass, FalseClass, Integer, Float,
 Array, Hash, Range, Regexp, Proc].each do |k|
  puts "#{k}#inspect -> #{k.instance_method(:inspect).owner}"
  puts "#{k}#hash -> #{k.instance_method(:hash).owner}"
end
p :unknown.method(:inspect).owner
p nil.method(:inspect).owner
p "s".method(:inspect).owner
p (1..2).method(:to_s).owner
p Object.instance_method(:method).bind_call(:unknown, :inspect).owner
