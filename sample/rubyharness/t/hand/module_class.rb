p Comparable.class
p Enumerable.class
p Kernel.class
p Module.class
p Class.class
p Integer.class
module Foo; end
p Foo.class
class Bar; end
p Bar.class
p Comparable.is_a?(Module)
p Comparable.is_a?(Class)
p Bar.is_a?(Module)
p Bar.is_a?(Class)
p Foo.instance_of?(Module)
