r = []; (1..10).step(-1) { |x| r << x }; p r
r2 = []; (10..1).step(-1) { |x| r2 << x }; p r2
r3 = []; (10..1).step(-2) { |x| r3 << x }; p r3
r4 = []; (10...1).step(-2) { |x| r4 << x }; p r4
r5 = []; (1..10).step(2) { |x| r5 << x }; p r5
r6 = []; (3.0..1.0).step(-0.5) { |x| r6 << x }; p r6
r7 = []; (1.0..3.0).step(-0.5) { |x| r7 << x }; p r7
def t; yield; rescue ArgumentError; "AE"; end
p t { (1..10).step(0) { |x| } }
