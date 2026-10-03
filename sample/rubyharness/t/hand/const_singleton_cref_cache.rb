# constants read in `class << self` bodies / methods: the answer depends on which singleton is the cref
module Outer
  X = :outer
  class A
    X = :a
    class << self
      def x = X
      def y = Y rescue :no_y
      def z = [X, Outer::X]
    end
  end
  class B < A
    X = :b
    class << self
      def x2 = X
    end
  end
end
r = []
3.times { r << Outer::A.x << Outer::B.x << Outer::B.x2 << Outer::A.y << Outer::A.z }
p r
Outer::A.const_set(:Y, :late)                       # defined later: the cached miss must not stick
p Outer::A.y, Outer::B.y
Outer::A.send(:remove_const, :X)                    # removed: falls back to the enclosing scope
p Outer::A.x, Outer::B.x
o1 = Object.new; o2 = Object.new
[o1, o2].each_with_index do |o, i|
  o.singleton_class.const_set(:K, i)
  class << o
    def k = K
  end
end
p 3.times.map { [o1.k, o2.k] }
GC.start
p [o1.k, o2.k, Outer::B.x2]
