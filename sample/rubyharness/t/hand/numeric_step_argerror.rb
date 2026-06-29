def t; yield; rescue ArgumentError; "AE"; end
r = []; 1.step(10, 2) { |x| r << x }; p r
r = []; 1.step(to: 5) { |x| r << x }; p r
r = []; 1.step(by: 2, to: 7) { |x| r << x }; p r
p t { 1.step(10, to: 20) { } }
p t { 1.step(10, 2, by: 3) { } }
p 1.step(10, 3).to_a
