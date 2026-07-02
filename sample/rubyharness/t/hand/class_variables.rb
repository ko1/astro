# Module#class_variable_get/set/defined?/class_variables reflection. vs ruby.
class A
  @@x = 1
  @@y = 2
end
class B < A
  @@z = 3
end
p A.class_variable_get(:@@x)
p A.class_variable_get("@@y")
A.class_variable_set(:@@x, 10)
p A.class_variable_get(:@@x)
A.class_variable_set(:@@fresh, 42)
p A.class_variable_get(:@@fresh)
p A.class_variable_defined?(:@@x)
p A.class_variable_defined?(:@@nope)
p A.class_variables.sort
p B.class_variables.sort
p B.class_variables(false)
begin
  A.class_variable_get(:@@missing)
rescue NameError => e
  p e.class
end
# set via subclass updates the ancestor-owned var
B.class_variable_set(:@@x, 100)
p A.class_variable_get(:@@x)
