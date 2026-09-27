eval 'template2 = "two"', TOPLEVEL_BINDING
b = TOPLEVEL_BINDING.dup
b.local_variable_set(:template2, "X")
p TOPLEVEL_BINDING.local_variable_get(:template2)
b.eval("template2 = 'Y'")
p TOPLEVEL_BINDING.local_variable_get(:template2)
TOPLEVEL_BINDING.local_variable_set(:template2, "Z")
p b.local_variable_get(:template2)
TOPLEVEL_BINDING.local_variable_set(:late, 1)
p b.local_variable_defined?(:late)
b.local_variable_set(:mine, 1)
c = b.dup
c.local_variable_set(:mine, 2)
p b.local_variable_get(:mine)
def m
  eval 'template3 = "three"', TOPLEVEL_BINDING
  b = TOPLEVEL_BINDING.dup
  b.local_variable_set(:foo, 1)
  p TOPLEVEL_BINDING.local_variable_defined?(:foo)
  c = TOPLEVEL_BINDING.dup
  c.eval("bar = 1")
  p TOPLEVEL_BINDING.local_variable_defined?(:bar), c.local_variable_get(:bar)
  d = TOPLEVEL_BINDING.eval("tap {|;template3| break binding}")
  d.local_variable_set(:template3, "THREE")
  p TOPLEVEL_BINDING.local_variable_get(:template3), d.local_variable_get(:template3)
end
m
