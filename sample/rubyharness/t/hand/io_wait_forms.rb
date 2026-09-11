# IO#wait (io/wait): (events, timeout) answers the ready mask; the legacy Symbol form
# takes at most one timeout and answers self / nil. vs ruby.
require "io/wait"
def try; yield; rescue Exception => e; [e.class, e.message]; end
r, w = IO.pipe
p w.wait(IO::WRITABLE, 0), r.wait(IO::READABLE, 0)
w.write("data"); p r.wait(IO::READABLE, 2), r.wait(1.5, 0)
p w.wait(0, :w).equal?(w), w.wait(:w).equal?(w), w.wait(2, :w, :r).equal?(w), r.wait(0.0, :r).equal?(r)
p w.wait(4, nil), w.wait(4, 0), w.wait(0.5)
f = File.new(__FILE__)
p f.wait(0, :r).equal?(f), f.wait(0, :rw).equal?(f), f.wait(:r, 0, :w).equal?(f), f.wait(0, :read_write, :readable_writable).equal?(f)
p try { w.wait(0, 0) }, try { w.wait(-1, 0) }, try { w.wait(nil, 0) }, try { w.wait("a", 0) }
p try { w.wait(:x) }, try { w.wait(0, 0, :r) }, try { w.wait("a") }, try { w.wait(-1) }, try { w.wait(:w, nil) }
f.close; p try { f.wait(IO::READABLE, 0) }, try { f.wait(0, :r) }
r.close; w.close
