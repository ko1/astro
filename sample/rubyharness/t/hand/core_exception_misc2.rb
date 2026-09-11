# Exception#exception(msg) clones without re-running the subclass #initialize;
# Errno subclasses inherit the errno/message; UncaughtThrowError#tag/#value;
# private method_missing / remove_const.
class CustomArgumentError < StandardError
  attr_reader :val
  def initialize(val)
    @val = val
    super("orig")
  end
end
e = CustomArgumentError.new(:boom)
e2 = e.exception("message")
p e2.class, e2.val, e2.message, e.message, e2.equal?(e)
p e.exception.equal?(e)

c = Class.new(Errno::ENOENT)
begin
  raise c, "custom message"
rescue => ex
  p ex.message, ex.errno, ex.class == c
end
p Class.new(Errno::EACCES).new.message

begin
  throw :abc
rescue UncaughtThrowError => ex
  p ex.tag, ex.value, ex.message
end
begin
  throw "s", 42
rescue UncaughtThrowError => ex
  p ex.tag, ex.value
end

p BasicObject.private_instance_methods(false).include?(:method_missing)
p Module.private_methods.include?(:remove_const)
p Object.public_instance_methods.include?(:remove_const)
class Foo
  def method_missing(n, *a) = [:mm, n, a]
end
p Foo.new.bar(1)
p Foo.new.respond_to?(:method_missing)
p Foo.new.respond_to?(:method_missing, true)
begin
  Object.new.method_missing(:x)
rescue NoMethodError => ex
  puts ex.message[/private method 'method_missing'/]
end
X = 1
p Object.send(:remove_const, :X)
begin
  Object.remove_const(:Y)
rescue NoMethodError => ex
  puts ex.message[/private method 'remove_const'/]
end
