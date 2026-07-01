# instance_variable_defined? is membership, not value: an ivar set to nil is
# still defined. remove_instance_variable truly removes it from the shape (so
# instance_variables and defined? stay consistent, and re-adding works). vs ruby.
class C
  def initialize; @a = 1; @b = nil; @c = 3; end
end
c = C.new
p c.instance_variables
p [c.instance_variable_defined?(:@a), c.instance_variable_defined?(:@b), c.instance_variable_defined?(:@c), c.instance_variable_defined?(:@z)]
p c.instance_variable_get(:@b)

o = Object.new
o.instance_variable_set(:@x, nil)
p o.instance_variable_defined?(:@x)
o.instance_variable_set(:@y, 42)
p o.remove_instance_variable(:@x)
p o.instance_variables
p o.instance_variable_defined?(:@x)
p o.instance_variable_get(:@x)
p [o.instance_variable_defined?(:@y), o.instance_variable_get(:@y)]
begin; o.remove_instance_variable(:@gone); rescue => e; p e.class; end

# middle remove keeps order; re-add works
d = Object.new
d.instance_variable_set(:@p, 1); d.instance_variable_set(:@q, 2); d.instance_variable_set(:@r, 3)
p d.remove_instance_variable(:@q)
p d.instance_variables
d.instance_variable_set(:@q, 20)
p d.instance_variables
p [d.instance_variable_get(:@p), d.instance_variable_get(:@q), d.instance_variable_get(:@r)]

# remove all then re-add
d.remove_instance_variable(:@p); d.remove_instance_variable(:@q); d.remove_instance_variable(:@r)
p d.instance_variables
d.instance_variable_set(:@fresh, 9)
p [d.instance_variables, d.instance_variable_get(:@fresh)]

# class-level ivars (side hash)
class K; @cv = nil; end
p K.instance_variable_defined?(:@cv)
p K.remove_instance_variable(:@cv)
p K.instance_variable_defined?(:@cv)
