# NameError/NoMethodError name the receiver as "an instance of Foo" unless a
# singleton class has actually been MATERIALIZED for it (`def obj.m`, #extend,
# #singleton_class, `class << obj`), in which case CRuby shows "#<Foo:0x…>".
# koruby materializes one for instance_eval/instance_exec (it needs it as the
# default definee), so every miss inside such a block read "#<Foo:0x…>" —
# rspec-core's aggregate_failures spec asserts the NameError text.
class Y; end
def clean(s) = s.sub(/0x\h+/, "0xADDR")
def probe(o) = clean((o.instance_eval { nope_x } rescue $!.message))
def probe_exec(o) = clean((o.instance_exec { nope_y } rescue $!.message))
def probe_send(o) = clean((o.send(:nope_z) rescue $!.message))

p [probe(Y.new), probe_exec(Y.new), probe_send(Y.new)]
y = Y.new; y.instance_eval { 1 }; p probe(y)                     # twice: the mark must survive
y = Y.new; y.instance_eval { @a = 1 }; p probe(y)                # ivars do not materialize one
y = Y.new; y.instance_variable_set(:@a, 1); p probe(y)
y = Y.new; y.dup; p probe(y)
y = Y.new; y.freeze; p probe(y)
y = Y.new; y.singleton_class; p probe(y)                          # …these do
y = Y.new; def y.extra; end; p probe(y)
y = Y.new; y.define_singleton_method(:m) { 1 }; p probe(y)
y = Y.new; y.extend(Module.new); p probe(y)
y = Y.new; class << y; end; p probe(y)
y = Y.new; y.instance_eval { def later; end }; p probe(y)         # a def inside materializes it
p [probe(Object.new), probe("str"), probe([1]), probe(Y), probe(nil), probe(1)]
anon = Class.new
p probe(anon.new)
class Z; def inspect = "ZZZ"; end
p probe(Z.new)
