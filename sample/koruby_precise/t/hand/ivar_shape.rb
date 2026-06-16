# shape transitions: same class, ivars added in different orders → distinct
# shapes; post-hoc ivar addition; deep ivar chains.
class Flex
  def initialize(order)
    if order == :xy
      @x = 1; @y = 2
    else
      @y = 20; @x = 10        # different add order → different shape, same names
    end
  end
  def x = @x
  def y = @y
  def add_z(v); @z = v; end   # transition adding a 3rd ivar
  def z = @z
end
a = Flex.new(:xy)
b = Flex.new(:yx)
p [a.x, a.y, b.x, b.y]        # [1,2,10,20] — order-independent by name
a.add_z(99); p [a.x, a.y, a.z, b.z]   # b.z still nil (no @z on b)
b.add_z(88); p [a.z, b.z]

# many distinct ivars (deep shape chain), read after all set
class Deep
  def initialize
    @v0=0;@v1=1;@v2=2;@v3=3;@v4=4;@v5=5;@v6=6;@v7=7;@v8=8;@v9=9
    @v10=10;@v11=11;@v12=12
  end
  def all; [@v0,@v3,@v7,@v12,@v9]; end
end
p Deep.new.all

# instance_variable_set adds new ivars dynamically (runtime transitions)
class Bag; end
bag = Bag.new
%i[a b c d].each_with_index { |n, i| bag.instance_variable_set(:"@#{n}", i * 11) }
p %i[a b c d].map { |n| bag.instance_variable_get(:"@#{n}") }
p bag.instance_variable_get(:@missing)   # nil

# reassigning existing ivars must NOT add a shape (stay same shape)
class P2; def initialize; @a=0; @b=0; end; def set(x,y); @a=x; @b=y; end; def g; [@a,@b]; end; end
q = P2.new
50.times { |i| q.set(i, i*2) }
p q.g
