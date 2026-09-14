# A top-level `def` goes into the VM's global function table, which a bare-name
# call reads before Object's method table.  Defining the same name on Object
# afterwards (define_method / alias_method / attr_*) has to win, or the old body
# keeps running silently.  Run under CRuby and koruby and diff.
def a1; "def1"; end
def a1; "def2"; end
p ["plain redefinition", a1]

def b1; "OLD"; end
Object.send(:define_method, :b1) { "DM" }
p ["define_method over def", b1, send(:b1), method(:b1).call]

def c1; "C"; end
Object.send(:alias_method, :c2, :c1)
p ["alias_method on Object", c2]

def d1; "D"; end
Object.send(:define_method, :d1) { "DM2" }
def d1; "D3"; end
p ["def after define_method", d1]

Object.send(:attr_accessor, :e1)
self.e1 = 7
p ["attr_accessor on Object", e1]

# the builtins share that table; unlinking must not reach them
p ["builtins still work", [1, 2].map { |x| x }.size, format("%d", 3), [1].include?(1)]
