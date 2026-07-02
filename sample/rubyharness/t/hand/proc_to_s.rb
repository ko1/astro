# Proc#to_s / #inspect: "#<Proc:0x.. file:line (lambda)>" with source location.
# vs ruby. Drop the non-deterministic address by keeping only the part after the
# first space (no regex — koruby's Regexp is pending).
tail = ->(s) { s.split(" ", 2)[1] }
pr = proc { }
p tail.(pr.to_s)
p tail.(pr.inspect)
la = lambda { |x| x }
p tail.(la.to_s)
ar = ->(a, b) { a + b }
p tail.(ar.to_s)
blk = nil
[1].each { blk = proc { } }
p tail.(blk.to_s)
p pr.to_s.start_with?("#<Proc:0x")
