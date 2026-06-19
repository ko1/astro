# Binding: capture local scope, local_variable_*, eval(str, binding), TOPLEVEL_BINDING.
# Diff-tested against CRuby.

# --- basics ---
x = 10
y = "hi"
b = binding
p b.class
p b.local_variable_get(:x)
p b.local_variable_get(:y)
p b.local_variable_get("x")
p b.local_variables.sort
p b.local_variable_defined?(:x)
p b.local_variable_defined?(:nope)
p b.receiver.class

# --- write back to an existing local ---
b.local_variable_set(:x, 99)
p x
p b.local_variable_get(:x)

# --- add a new local via the API ---
b.local_variable_set(:added, 7)
p b.local_variable_get(:added)
p b.local_variables.sort

# --- binding escaping its defining frame ---
def make_b(n)
  loc = n * 3
  s = "v#{n}"
  binding
end
eb = make_b(4)
p eb.local_variable_get(:loc)
p eb.local_variable_get(:s)
p eb.local_variable_get(:n)

# --- binding shares the frame with a closure (mutation both ways) ---
def shared
  count = 0
  inc = -> { count += 1 }
  bb = binding
  inc.call
  inc.call
  [bb.local_variable_get(:count), count]
end
p shared

# --- eval(str, binding) reads/writes the binding's locals ---
a1 = 1
a2 = 2
eb2 = binding
p eval("a1 + a2", eb2)
eval("a1 = a1 + 100", eb2)
p a1
p eb2.local_variable_get(:a1)
p eval("nw = a1 * 2; nw", eb2)
p eb2.local_variable_get(:nw)

# --- eval over a method (closed) binding ---
def mk
  m1 = 3
  m2 = 4
  binding
end
mb = mk
eval("m1 = m1 * 10", mb)
p mb.local_variable_get(:m1)
p eval("m1 + m2", mb)

# --- eval against an API-added local ---
eb3 = binding
eb3.local_variable_set(:dyn, 9)
p eval("dyn + 1", eb3)

# --- nested eval ---
nx = 5
nb = binding
p eval("eval('nx * 2', binding)", nb)

# --- TOPLEVEL_BINDING ---
p TOPLEVEL_BINDING.class
p eval("1 + 1", TOPLEVEL_BINDING)
p eval("to_s", TOPLEVEL_BINDING)
