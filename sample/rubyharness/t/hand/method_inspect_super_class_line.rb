class A; def f(a) = 1; end; class B < A; def f(a, b = 2) = 2; end
p B.new.method(:f).super_method.inspect.sub(/ \S+:\d+>/, ">"), B.instance_method(:f).super_method.inspect.sub(/ \S+:\d+>/, ">")
p B.new.method(:f).inspect.sub(/ \S+:\d+>/, ">"), B.new.method(:f).dup.inspect.sub(/ \S+:\d+>/, ">")
o = Object.new; m2 = Module.new { def to_s = "p" }; o.singleton_class.prepend(m2)
p o.method(:to_s).super_method.inspect
class Dog; end; mm = Module.new { def new(*) = super }; Dog.singleton_class.prepend(mm)
p Dog.method(:new).super_method.inspect
class TT9; end
begin
  module TT9; end
rescue TypeError => e
  p e.message, e.backtrace.first
end
module MM9; end
begin
  class MM9; end
rescue TypeError => e
  p e.message, e.backtrace.first
end
