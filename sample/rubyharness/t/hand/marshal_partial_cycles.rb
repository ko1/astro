# Marshal.load registers an object before its contents as unfinished (CRuby's partial
# objects): a self-link inside it neither freezes it early nor fires the load proc.
# Plain objects, Structs, exceptions and Sets; Set's dump is CRuby's compat form.
class O; attr_accessor :x; end
o = O.new; o.x = o
l = Marshal.load(Marshal.dump(o), freeze: true); p [l.frozen?, l.x.equal?(l)] rescue p [$!.class]
S = Struct.new(:m); s = S.new; s.m = s
l = Marshal.load(Marshal.dump(s), freeze: true); p [l.frozen?, l.m.equal?(l)] rescue p [$!.class]
calls = []; l = Marshal.load(Marshal.dump(o), ->(v) { calls << v.class; v }); p calls, l.x.equal?(l)
calls = []; Marshal.load(Marshal.dump(s), ->(v) { calls << v.class; v }); p calls
a = [1]; a << a; p Marshal.load(Marshal.dump(a), freeze: true).then { |v| [v.frozen?, v[1].equal?(v)] }
h = {}; h[:s] = h; p Marshal.load(Marshal.dump(h), freeze: true).then { |v| [v.frozen?, v[:s].equal?(v)] }
e = RuntimeError.new("x"); e.instance_variable_set(:@me, e); v = Marshal.load(Marshal.dump(e)); p v.instance_variable_get(:@me).equal?(v)
s = Set[1]; s.instance_variable_set(:@me, 5); p Marshal.dump(s)
p Marshal.dump(Set[1].compare_by_identity)
class SS < Set; def initialize(*) = raise("no init"); def add(x) = raise("no add"); end
p Marshal.dump(SS[1]) rescue p $!
p Marshal.load(Marshal.dump(Set[1, "a"].compare_by_identity)).then { |v| [v.compare_by_identity?, v.size] }
p Marshal.load("\x04\bo:\bSet\x06:\n@hash}\x06i\x06TF".b)
v = Marshal.load(Marshal.dump(s)); p v, v.instance_variable_get(:@me)
s = Set[1]; s.instance_variable_set(:@me, s); v = Marshal.load(Marshal.dump(s)); p v.instance_variable_get(:@me).equal?(v), v.to_a
p Marshal.load(Marshal.dump(Set[1]), freeze: true).frozen?
p Marshal.load(Marshal.dump([Set[1], Set[2]]))
