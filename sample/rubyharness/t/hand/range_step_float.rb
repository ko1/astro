out = []; (1..3).step(0.5) { |x| out << x }; p out
out = []; (1.0..3.0).step(0.5) { |x| out << x }; p out
out = []; (1.0...3.0).step(0.5) { |x| out << x }; p out
out = []; (1..10).step(2) { |x| out << x }; p out
out = []; (1.0..3.0).step(1) { |x| out << x }; p out
p (begin; (1..5).step(0.0) { }; rescue ArgumentError; "AE"; end)
