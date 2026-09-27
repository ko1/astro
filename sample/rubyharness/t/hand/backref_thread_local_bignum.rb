def a_m(q1, q2)
  "aaa" =~ /(a+)/
  q1 << :a_in; q2.pop          # switch to B while inside a backref method
  $1
end
def b_m(q1, q2)
  "bbb" =~ /(b+)/
  q1.pop                       # wait until A is inside
  q2 << :go
  $1
end
"top" =~ /(t)op/
q1 = Queue.new; q2 = Queue.new
ta = Thread.new { r = a_m(q1, q2); [r, $~&.to_a] }
tb = Thread.new { r = b_m(q1, q2); sleep 0.01; [r, $~&.to_a] }
p ta.value, tb.value, $~.to_a
"a" =~ /(.)/
p $4294967296, $1073741823, $1073741824, defined?($4294967296), defined?($1)
