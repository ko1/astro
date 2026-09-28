require 'date'
k = Class.new(Date)
a = Date.new(2008,10,10); b = k.new(2008,10,10); dt = DateTime.new(2008,10,10)
p a.eql?(b), b.eql?(a), a.eql?(dt), dt.eql?(a), a == dt, a.hash == b.hash, a.hash == dt.hash
p({a => 1}[b], {a => 1}[dt])
p b.inspect.sub(/0x\h+/, "X")
