# Ractor on green threads: new/value (RemoteError with the cause, value once), send /
# receive, Port, monitor, select, make_shareable / shareable? (deep freeze), ractor-local
# storage, the class hierarchy.  No isolation is enforced (docs/todo.md).
def t = (yield rescue [$!.class, $!.message[0, 60], ($!.cause.class rescue nil)])
p Ractor.current.inspect, Ractor.main?, Ractor.count
r = Ractor.new(1, 2) { |a, b| a + b }; p r.value, t { r.value }
p Ractor.new { self.class }.value
o = [1, "x"]; r2 = Ractor.new(o) { |x| x }; v2 = r2.value; p v2 == o, v2.equal?(o)
f = [1, "x".freeze].freeze; p Ractor.new(f) { |x| x }.value.equal?(f)
r3 = Ractor.new { Ractor.receive * 2 }; r3.send(21); p r3.value
pt = Ractor::Port.new; r4 = Ractor.new(pt) { |q| q << 1; q << 2; :done }; p pt.receive, pt.receive, r4.value, pt.closed?
pt2 = Ractor::Port.new; pt2.close; p t { pt2 << 1 }
mp = Ractor::Port.new; r5 = Ractor.new { Ractor.receive }; r5.monitor(mp); r5 << 8; p r5.value, mp.receive
p Ractor.select(Ractor::Port.new.tap { |x| x << :v }).last
p Ractor.make_shareable({a: [1, {b: "c"}]}).then { |h| Ractor.shareable?(h) }, Ractor.shareable?(Object.new.freeze), Ractor.shareable?(String), Ractor.shareable?("x"), Ractor.shareable?(:s)
x = Object.new; x.instance_variable_set(:@a, +"s"); Ractor.make_shareable(x); p x.frozen?, x.instance_variable_get(:@a).frozen?
p((Ractor.make_shareable(proc { 1 }) rescue $!.class))
p Ractor.current[:k], (Ractor.current[:k] = 5), Ractor.current[:k], Ractor.store_if_absent(:z) { 9 }, Ractor.store_if_absent(:z) { 10 }
p Ractor.new(name: "w") { 1 }.name, t { Ractor.new }
