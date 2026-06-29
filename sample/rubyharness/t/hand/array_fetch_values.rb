a = [1, 2, 3]
p a.fetch_values(0, 2)
class TI; def to_int; 1; end; end
p a.fetch_values(TI.new)
p a.fetch_values(0, 44) { |i| "missing:#{i}" }
def t; yield; rescue IndexError; "IE"; rescue TypeError; "TE"; end
p t { a.fetch_values(0, 44) }
p t { a.fetch_values(Object.new) }
