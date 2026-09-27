r = []; 2.times { |i| r << /#{i}/o }; p r, r[0].equal?(r[1])
o = Object.new; def o.to_s = (puts "to_s"; "x"); 2.times { /#{o}/o }
def m(x) = /a#{x}b/o
p m(1), m(2)
3.times { |i| p((/#{i}/io).source) }
p("A1" =~ /a#{1}/io)
