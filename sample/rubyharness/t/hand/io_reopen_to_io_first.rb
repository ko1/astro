# IO#reopen(obj): one argument that answers #to_io is an IO (even when it also
# answers #to_path); the receiver takes the donor's class and loses its
# singleton class, as RBASIC_SET_CLASS does.
o = Object.new; def o.to_io = File.open(__FILE__); def o.to_path = "/dev/null"
io = STDERR.dup; io.reopen(o); p io.class, io.is_a?(File)
io2 = STDERR.dup; def io2.x = 1; io2.reopen(File.open(__FILE__)); p io2.class, io2.is_a?(File), (io2.x rescue $!.class)
w = Object.new; def w.to_path = "/dev/null"
io3 = STDERR.dup; io3.reopen(w, "w"); p io3.class
