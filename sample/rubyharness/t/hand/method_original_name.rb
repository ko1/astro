class C
  def foo; 1; end
  alias_method :bar, :foo
  alias_method :baz, :bar
end
p C.instance_method(:foo).original_name
p C.instance_method(:bar).original_name
p C.instance_method(:baz).original_name
p C.new.method(:bar).original_name
p C.new.method(:foo).name
p C.new.method(:bar).name
