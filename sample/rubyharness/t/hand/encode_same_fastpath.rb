# String#encode / #encode! to the string's own encoding (returns a copy / self, frozen raises), names and aliases,
# Encoding.find's memo (special names follow default_internal / default_external), ascii_compatible? / dummy?
# memo, WeakKeyMap keys that are never collectable, and Monitor#synchronize re-entry / ownership
require 'monitor'
s = "abc"
a = s.encode("UTF-8"); b = s.encode(Encoding::UTF_8); c = s.encode("utf-8")
p a, a.equal?(s), b.equal?(s), c.encoding, a.frozen?, "x".freeze.encode("UTF-8").frozen?
p "d".encode("US-ASCII").encoding, "e".b.encode("ASCII-8BIT").encoding, "f".b.encode("BINARY").encoding, "g".encode("UTF-16LE").bytesize
inv = "\xff".b.force_encoding("UTF-8"); p inv.encode("UTF-8").valid_encoding?, inv.encode("UTF-8", invalid: :replace).valid_encoding?
t = +"h"; p t.encode!("UTF-8").equal?(t), t.encode!(Encoding::US_ASCII).encoding, (("i".freeze).encode!("UTF-8") rescue $!.class)
p Encoding.find("UTF-8"), Encoding.find("utf-8"), Encoding.find("UTF-8"), Encoding.find("binary"), (Encoding.find("nope") rescue $!.class)
p Encoding.find("internal"); Encoding.default_internal = "UTF-8"; p Encoding.find("internal"); Encoding.default_internal = nil; p Encoding.find("internal")
p Encoding.find("external"), Encoding.find("locale")
3.times { p [Encoding::UTF_8.ascii_compatible?, Encoding::UTF_16LE.ascii_compatible?, Encoding::UTF_16.dummy?, Encoding::ISO_2022_JP.ascii_compatible?] }
w = ObjectSpace::WeakKeyMap.new; k = "key"
w[k] = 1; p w[k], w[1], w[1.5], w[2**70], w[:sym], w[nil], w.key?(k)
[1, 2.5, 2**70, :s, nil, true].each { |x| p(begin; w[x] = 1; rescue => e; [e.class, e.message]; end) }
m = Monitor.new
p m.synchronize { m.synchronize { m.mon_owned? } }, m.mon_owned?, m.mon_locked?
p((Thread.new { Thread.current.report_on_exception = false; m.mon_exit }.join rescue [$!.class, $!.message]))
q = Queue.new; th = Thread.new { m.synchronize { q << :in; sleep 0.05; :done } }; q.pop; p m.mon_locked?, th.value, m.mon_locked?
p((m.synchronize { raise "boom" } rescue $!.message), m.mon_locked?)
# no target: a copy / nothing while default_internal is unset, a conversion once it is set
na = +"na"; p na.encode, na.encode.equal?(na), na.encode!.equal?(na), na.encoding, (("nf".freeze).encode! rescue $!.class)
Encoding.default_internal = "US-ASCII"; nb = +"nb"; p nb.encode.encoding, nb.encode!.encoding; Encoding.default_internal = nil
