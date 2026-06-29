a = [1, 2, 3]
p a.fetch(1)
p a.fetch(-1)
class TI; def to_int; 2; end; end
p a.fetch(TI.new)
p a.fetch(10, :default)
p a.fetch(10) { |i| "block:#{i}" }
o = Object.new
def o.to_int; 10; end
p a.fetch(o) { |i| i.equal?(o) ? "got-original" : "got-#{i}" }
def t; yield; rescue IndexError; "IE"; rescue TypeError; "TE"; end
p t { a.fetch(10) }
p t { a.fetch("x") }
