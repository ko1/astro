# methods koruby keeps on a core class report CRuby's owner (Comparable, Enumerable, Numeric, Kernel)
p 1.method(:clamp).owner, Integer.instance_method(:between?).owner, [].method(:map).owner, [].method(:each_slice).owner, 1.method(:+).owner
p 1.method(:clamp), Integer.instance_method(:clamp), [].method(:each_slice)
p Integer.instance_methods(false).include?(:clamp), Integer.instance_methods.include?(:clamp), Comparable.instance_methods(false).include?(:clamp)
p Array.instance_methods(false).include?(:each_slice), Hash.instance_methods(false).include?(:map), Range.instance_methods(false).include?(:sum)
class MyInt; include Comparable; def <=>(o) = 0; end
p MyInt.instance_method(:clamp).owner
